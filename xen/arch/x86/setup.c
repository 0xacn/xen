
#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/serial.h>
#include <xen/softirq.h>
#include <xen/acpi.h>
#include <xen/console.h>
#include <xen/serial.h>
#include <xen/trace.h>
#include <xen/multiboot.h>
#include <asm/bitops.h>
#include <asm/smp.h>
#include <asm/processor.h>
#include <asm/mpspec.h>
#include <asm/apic.h>
#include <asm/desc.h>
#include <asm/domain_page.h>
#include <asm/shadow.h>
#include <asm/e820.h>

/*
 * opt_xenheap_megabytes: Size of Xen heap in megabytes, excluding the
 * pfn_info table and allocation bitmap.
 */
static unsigned int opt_xenheap_megabytes = XENHEAP_DEFAULT_MB;
#if defined(CONFIG_X86_64)
integer_param("xenheap_megabytes", opt_xenheap_megabytes);
#endif

/* opt_noht: If true, Hyperthreading is ignored. */
int opt_noht = 0;
boolean_param("noht", opt_noht);

/* opt_watchdog: If true, run a watchdog NMI on each processor. */
static int opt_watchdog = 0;
boolean_param("watchdog", opt_watchdog);

/* **** Linux config option: propagated to domain0. */
/* "acpi=off":    Sisables both ACPI table parsing and interpreter. */
/* "acpi=force":  Override the disable blacklist.                   */
/* "acpi=strict": Disables out-of-spec workarounds.                 */
/* "acpi=ht":     Limit ACPI just to boot-time to enable HT.        */
/* "acpi=noirq":  Disables ACPI interrupt routing.                  */
static void parse_acpi_param(char *s);
custom_param("acpi", parse_acpi_param);

/* **** Linux config option: propagated to domain0. */
/* acpi_skip_timer_override: Skip IRQ0 overrides. */
extern int acpi_skip_timer_override;
boolean_param("acpi_skip_timer_override", acpi_skip_timer_override);

/* **** Linux config option: propagated to domain0. */
/* noapic: Disable IOAPIC setup. */
extern int skip_ioapic_setup;
boolean_param("noapic", skip_ioapic_setup);

int early_boot = 1;

unsigned long xenheap_phys_end;

extern void arch_init_memory(void);
extern void init_IRQ(void);
extern void trap_init(void);
extern void time_init(void);
extern void ac_timer_init(void);
extern void initialize_keytable();
extern int do_timer_lists_from_pit;

struct cpuinfo_x86 boot_cpu_data = { 0, 0, 0, 0, -1 };

#if defined(CONFIG_X86_64)
unsigned long mmu_cr4_features = X86_CR4_PSE | X86_CR4_PGE | X86_CR4_PAE;
#else
unsigned long mmu_cr4_features = X86_CR4_PSE | X86_CR4_PGE;
#endif
EXPORT_SYMBOL(mmu_cr4_features);

unsigned long wait_init_idle;

struct exec_domain *idle_task[NR_CPUS] = { &idle0_exec_domain };

int acpi_disabled;

int phys_proc_id[NR_CPUS];
int logical_proc_id[NR_CPUS];

/* Standard macro to see if a specific flag is changeable. */
static inline int flag_is_changeable_p(unsigned long flag)
{
    unsigned long f1, f2;

    asm("pushf\n\t"
        "pushf\n\t"
        "pop %0\n\t"
        "mov %0,%1\n\t"
        "xor %2,%0\n\t"
        "push %0\n\t"
        "popf\n\t"
        "pushf\n\t"
        "pop %0\n\t"
        "popf\n\t"
        : "=&r" (f1), "=&r" (f2)
        : "ir" (flag));

    return ((f1^f2) & flag) != 0;
}

/* Probe for the CPUID instruction */
static int __init have_cpuid_p(void)
{
    return flag_is_changeable_p(X86_EFLAGS_ID);
}

void __init get_cpu_vendor(struct cpuinfo_x86 *c)
{
    char *v = c->x86_vendor_id;

    if (!strcmp(v, "GenuineIntel"))
        c->x86_vendor = X86_VENDOR_INTEL;
    else if (!strcmp(v, "AuthenticAMD"))
        c->x86_vendor = X86_VENDOR_AMD;
    else if (!strcmp(v, "CyrixInstead"))
        c->x86_vendor = X86_VENDOR_CYRIX;
    else if (!strcmp(v, "UMC UMC UMC "))
        c->x86_vendor = X86_VENDOR_UMC;
    else if (!strcmp(v, "CentaurHauls"))
        c->x86_vendor = X86_VENDOR_CENTAUR;
    else if (!strcmp(v, "NexGenDriven"))
        c->x86_vendor = X86_VENDOR_NEXGEN;
    else if (!strcmp(v, "RiseRiseRise"))
        c->x86_vendor = X86_VENDOR_RISE;
    else if (!strcmp(v, "GenuineTMx86") ||
             !strcmp(v, "TransmetaCPU"))
        c->x86_vendor = X86_VENDOR_TRANSMETA;
    else
        c->x86_vendor = X86_VENDOR_UNKNOWN;
}

static void __init init_intel(struct cpuinfo_x86 *c)
{
    /* SEP CPUID bug: Pentium Pro reports SEP but doesn't have it */
    if ( c->x86 == 6 && c->x86_model < 3 && c->x86_mask < 3 )
        clear_bit(X86_FEATURE_SEP, &c->x86_capability);

#ifdef CONFIG_SMP
    if ( test_bit(X86_FEATURE_HT, &c->x86_capability) )
    {
        u32     eax, ebx, ecx, edx;
        int     initial_apic_id, siblings, cpu = smp_processor_id();
        
        cpuid(1, &eax, &ebx, &ecx, &edx);
        ht_per_core = siblings = (ebx & 0xff0000) >> 16;

        if ( opt_noht )
            clear_bit(X86_FEATURE_HT, &c->x86_capability[0]);

        if ( siblings <= 1 )
        {
            printk(KERN_INFO  "CPU#%d: Hyper-Threading is disabled\n", cpu);
        } 
        else if ( siblings > 2 )
        {
            panic("We don't support more than two logical CPUs per package!");
        }
        else
        {
            initial_apic_id = ebx >> 24 & 0xff;
            phys_proc_id[cpu]    = initial_apic_id >> 1;
            logical_proc_id[cpu] = initial_apic_id & 1;
            printk(KERN_INFO  "CPU#%d: Physical ID: %d, Logical ID: %d\n",
                   cpu, phys_proc_id[cpu], logical_proc_id[cpu]);
        }
    }
#endif

#ifdef CONFIG_VMX
    start_vmx();
#endif

}

static void __init init_amd(struct cpuinfo_x86 *c)
{
    /* Bit 31 in normal CPUID used for nonstandard 3DNow ID;
       3DNow is IDd by bit 31 in extended CPUID (1*32+31) anyway */
    clear_bit(0*32+31, &c->x86_capability);
	
    switch(c->x86)
    {
    case 5:
        panic("AMD K6 is not supported.\n");
    case 6:	/* An Athlon/Duron. We can trust the BIOS probably */
        break;		
    }
}

/*
 * This does the hard work of actually picking apart the CPU stuff...
 */
void __init identify_cpu(struct cpuinfo_x86 *c)
{
    int i, cpu = smp_processor_id();
    u32 xlvl, tfms, junk;

    phys_proc_id[cpu]    = cpu;
    logical_proc_id[cpu] = 0;

    c->x86_vendor = X86_VENDOR_UNKNOWN;
    c->cpuid_level = -1;	/* CPUID not detected */
    c->x86_model = c->x86_mask = 0;	/* So far unknown... */
    c->x86_vendor_id[0] = '\0'; /* Unset */
    memset(&c->x86_capability, 0, sizeof c->x86_capability);

    if ( !have_cpuid_p() )
        panic("Ancient processors not supported\n");

    /* Get vendor name */
    cpuid(0x00000000, (unsigned int *)&c->cpuid_level,
          (unsigned int *)&c->x86_vendor_id[0],
          (unsigned int *)&c->x86_vendor_id[8],
          (unsigned int *)&c->x86_vendor_id[4]);

    get_cpu_vendor(c);
		
    if ( c->cpuid_level == 0 )
        panic("Decrepit CPUID not supported\n");

    cpuid(0x00000001, &tfms, &junk, &junk,
          &c->x86_capability[0]);
    c->x86 = (tfms >> 8) & 15;
    c->x86_model = (tfms >> 4) & 15;
    c->x86_mask = tfms & 15;

    /* AMD-defined flags: level 0x80000001 */
    xlvl = cpuid_eax(0x80000000);
    if ( (xlvl & 0xffff0000) == 0x80000000 ) {
        if ( xlvl >= 0x80000001 )
            c->x86_capability[1] = cpuid_edx(0x80000001);
    }

    /* Transmeta-defined flags: level 0x80860001 */
    xlvl = cpuid_eax(0x80860000);
    if ( (xlvl & 0xffff0000) == 0x80860000 ) {
        if (  xlvl >= 0x80860001 )
            c->x86_capability[2] = cpuid_edx(0x80860001);
    }

    printk("CPU%d: Before vendor init, caps: %08x %08x %08x, vendor = %d\n",
           smp_processor_id(),
           c->x86_capability[0],
           c->x86_capability[1],
           c->x86_capability[2],
           c->x86_vendor);

    switch ( c->x86_vendor ) {
    case X86_VENDOR_INTEL:
        init_intel(c);
        break;
    case X86_VENDOR_AMD:
        init_amd(c);
        break;
    case X86_VENDOR_UNKNOWN:  /* Connectix Virtual PC reports this */
	break;
    case X86_VENDOR_CENTAUR:
        break;
    default:
        printk("Unknown CPU identifier (%d): continuing anyway, "
               "but might fail.\n", c->x86_vendor);
    }
	
    printk("CPU caps: %08x %08x %08x %08x\n",
           c->x86_capability[0],
           c->x86_capability[1],
           c->x86_capability[2],
           c->x86_capability[3]);

    /*
     * On SMP, boot_cpu_data holds the common feature set between
     * all CPUs; so make sure that we indicate which features are
     * common between the CPUs.  The first time this routine gets
     * executed, c == &boot_cpu_data.
     */
    if ( c != &boot_cpu_data ) {
        /* AND the already accumulated flags with these */
        for ( i = 0 ; i < NCAPINTS ; i++ )
            boot_cpu_data.x86_capability[i] &= c->x86_capability[i];
    }
}


unsigned long cpu_initialized;
void __init cpu_init(void)
{
    int nr = smp_processor_id();
    struct tss_struct *t = &init_tss[nr];
    char gdt_load[10];

    if ( test_and_set_bit(nr, &cpu_initialized) )
        panic("CPU#%d already initialized!!!\n", nr);
    printk("Initializing CPU#%d\n", nr);

    *(unsigned short *)(&gdt_load[0]) = LAST_RESERVED_GDT_BYTE;
    *(unsigned long  *)(&gdt_load[2]) = GDT_VIRT_START(current);
    __asm__ __volatile__ ( "lgdt %0" : "=m" (gdt_load) );

    /* No nested task. */
    __asm__ __volatile__ ( "pushf ; andw $0xbfff,(%"__OP"sp) ; popf" );

    /* Ensure FPU gets initialised for each domain. */
    stts();

    /* Set up and load the per-CPU TSS and LDT. */
    t->bitmap = IOBMP_INVALID_OFFSET;
#if defined(CONFIG_X86_32)
    t->ss0  = __HYPERVISOR_DS;
    t->esp0 = get_stack_bottom();
#elif defined(CONFIG_X86_64)
    /* Bottom-of-stack must be 16-byte aligned or CPU will force it! :-o */
    BUG_ON((get_stack_bottom() & 15) != 0);
    t->rsp0 = get_stack_bottom();
#endif
    set_tss_desc(nr,t);
    load_TR(nr);
    __asm__ __volatile__ ( "lldt %%ax" : : "a" (0) );

    /* Clear all 6 debug registers. */
#define CD(register) __asm__ ( "mov %0,%%db" #register : : "r" (0UL) );
    CD(0); CD(1); CD(2); CD(3); /* no db4 and db5 */; CD(6); CD(7);
#undef CD

    /* Install correct page table. */
    write_ptbase(current);

    init_idle_task();
}

int acpi_force;
char acpi_param[10] = "";
static void parse_acpi_param(char *s)
{
    /* Save the parameter so it can be propagated to domain0. */
    strncpy(acpi_param, s, sizeof(acpi_param));
    acpi_param[sizeof(acpi_param)-1] = '\0';

    /* Interpret the parameter for use within Xen. */
    if ( !strcmp(s, "off") )
    {
        disable_acpi();
    }
    else if ( !strcmp(s, "force") )
    {
        acpi_force = 1;
        acpi_ht = 1;
        acpi_disabled = 0;
    }
    else if ( !strcmp(s, "strict") )
    {
        acpi_strict = 1;
    }
    else if ( !strcmp(s, "ht") )
    {
        if ( !acpi_force )
            disable_acpi();
        acpi_ht = 1;
    }
    else if ( !strcmp(s, "noirq") )
    {
        acpi_noirq_set();
    }
}

static void __init do_initcalls(void)
{
    initcall_t *call;
    for ( call = &__initcall_start; call < &__initcall_end; call++ )
        (*call)();
}

static void __init start_of_day(void)
{
#ifdef MEMORY_GUARD
    /* Unmap the first page of CPU0's stack. */
    extern unsigned long cpu0_stack[];
    memguard_guard_stack(cpu0_stack);
#endif

    open_softirq(NEW_TLBFLUSH_CLOCK_PERIOD_SOFTIRQ, new_tlbflush_clock_period);

    if ( opt_watchdog ) 
        nmi_watchdog = NMI_LOCAL_APIC;

    sort_exception_tables();

    arch_do_createdomain(current);
    
    /* Map default GDT into their final position in the idle page table. */
    map_pages(
        idle_pg_table,
        GDT_VIRT_START(current) + FIRST_RESERVED_GDT_BYTE,
        virt_to_phys(gdt_table), PAGE_SIZE, __PAGE_HYPERVISOR);

    /* Process CPU type information. */
    identify_cpu(&boot_cpu_data);
    if ( cpu_has_fxsr )
        set_in_cr4(X86_CR4_OSFXSR);
    if ( cpu_has_xmm )
        set_in_cr4(X86_CR4_OSXMMEXCPT);

    find_smp_config();

    smp_alloc_memory();

    paging_init();

    acpi_boot_table_init();
    acpi_boot_init();

    if ( smp_found_config ) 
        get_smp_config();

    init_apic_mappings();

    scheduler_init();	

    init_IRQ();

    trap_init();

    time_init();

    arch_init_memory();

    smp_boot_cpus();

    __sti();

    initialize_keytable();

    serial_init_stage2();

    if ( !cpu_has_apic )
    {
        do_timer_lists_from_pit = 1;
        if ( smp_num_cpus != 1 )
            panic("We need local APICs on SMP machines!");
    }

    ac_timer_init();    /* init accurate timers */
    init_xen_time();	/* initialise the time */
    schedulers_start(); /* start scheduler for each CPU */

    check_nmi_watchdog();

    do_initcalls();

    wait_init_idle = cpu_online_map;
    clear_bit(smp_processor_id(), &wait_init_idle);
    smp_threads_ready = 1;
    smp_commence(); /* Tell other CPUs that state of the world is stable. */
    while ( wait_init_idle != 0 )
        cpu_relax();

    watchdog_enable();

#ifdef CONFIG_X86_64 /* x86_32 uses low mappings when building DOM0. */
    zap_low_mappings();
#endif
}

void __init __start_xen(multiboot_info_t *mbi)
{
    char *cmdline;
    module_t *mod = (module_t *)__va(mbi->mods_addr);
    void *heap_start;
    unsigned long firsthole_start, nr_pages;
    unsigned long initial_images_start, initial_images_end;
    struct e820entry e820_raw[E820MAX];
    int i, e820_raw_nr = 0, bytes = 0;

    /* Parse the command-line options. */
    if ( (mbi->flags & MBI_CMDLINE) && (mbi->cmdline != 0) )
        cmdline_parse(__va(mbi->cmdline));

    /* Must do this early -- e.g., spinlocks rely on get_current(). */
    set_current(&idle0_exec_domain);
    set_processor_id(0);

    /* We initialise the serial devices very early so we can get debugging. */
    serial_init_stage1();

    init_console();

    /* Check that we have at least one Multiboot module. */
    if ( !(mbi->flags & MBI_MODULES) || (mbi->mods_count == 0) )
    {
        printk("FATAL ERROR: Require at least one Multiboot module.\n");
        for ( ; ; ) ;
    }

    xenheap_phys_end = opt_xenheap_megabytes << 20;

    if ( mbi->flags & MBI_MEMMAP )
    {
        while ( bytes < mbi->mmap_length )
        {
            memory_map_t *map = __va(mbi->mmap_addr + bytes);
            e820_raw[e820_raw_nr].addr = 
                ((u64)map->base_addr_high << 32) | (u64)map->base_addr_low;
            e820_raw[e820_raw_nr].size = 
                ((u64)map->length_high << 32) | (u64)map->length_low;
            e820_raw[e820_raw_nr].type = 
                (map->type > E820_SHARED_PAGE) ? E820_RESERVED : map->type;
            e820_raw_nr++;
            bytes += map->size + 4;
        }
    }
    else if ( mbi->flags & MBI_MEMLIMITS )
    {
        e820_raw[0].addr = 0;
        e820_raw[0].size = mbi->mem_lower << 10;
        e820_raw[0].type = E820_RAM;
        e820_raw[1].addr = 0x100000;
        e820_raw[1].size = mbi->mem_upper << 10;
        e820_raw[1].type = E820_RAM;
        e820_raw_nr = 2;
    }
    else
    {
        printk("FATAL ERROR: Bootloader provided no memory information.\n");
        for ( ; ; ) ;
    }

    max_page = init_e820(e820_raw, &e820_raw_nr);

    /* Find the first high-memory RAM hole. */
    for ( i = 0; i < e820.nr_map; i++ )
        if ( (e820.map[i].type == E820_RAM) &&
             (e820.map[i].addr >= 0x100000) )
            break;
    firsthole_start = e820.map[i].addr + e820.map[i].size;

    /* Relocate the Multiboot modules. */
    initial_images_start = xenheap_phys_end;
    initial_images_end   = initial_images_start + 
        (mod[mbi->mods_count-1].mod_end - mod[0].mod_start);
    if ( initial_images_end > firsthole_start )
    {
        printk("Not enough memory to stash the DOM0 kernel image.\n");
        for ( ; ; ) ;
    }
#if defined(CONFIG_X86_32)
    memmove((void *)initial_images_start,  /* use low mapping */
            (void *)mod[0].mod_start,      /* use low mapping */
            mod[mbi->mods_count-1].mod_end - mod[0].mod_start);
#elif defined(CONFIG_X86_64)
    memmove(__va(initial_images_start),
            __va(mod[0].mod_start),
            mod[mbi->mods_count-1].mod_end - mod[0].mod_start);
#endif

    /* Initialise boot-time allocator with all RAM situated after modules. */
    heap_start = memguard_init(&_end);
    heap_start = __va(init_boot_allocator(__pa(heap_start)));
    nr_pages   = 0;
    for ( i = 0; i < e820.nr_map; i++ )
    {
        if ( e820.map[i].type != E820_RAM )
            continue;
        nr_pages += e820.map[i].size >> PAGE_SHIFT;
        if ( (e820.map[i].addr + e820.map[i].size) >= initial_images_end )
            init_boot_pages((e820.map[i].addr < initial_images_end) ?
                            initial_images_end : e820.map[i].addr,
                            e820.map[i].addr + e820.map[i].size);
    }

#if defined (CONFIG_X86_64)
    /* On x86/64 we can 1:1 map every registered memory area. */
    /* We use the raw_e820 map because we sometimes truncate the cooked map. */
    for ( i = 0; i < e820_raw_nr; i++ )
    {
        unsigned long min, sz;
        min = (unsigned long)e820_raw[i].addr &
            ~(((unsigned long)L1_PAGETABLE_ENTRIES << PAGE_SHIFT) - 1);
        sz  = ((unsigned long)e820_raw[i].size +
               ((unsigned long)L1_PAGETABLE_ENTRIES << PAGE_SHIFT) - 1) &
            ~(((unsigned long)L1_PAGETABLE_ENTRIES << PAGE_SHIFT) - 1);
        map_pages(idle_pg_table, PAGE_OFFSET + min, min, sz, PAGE_HYPERVISOR);
    }
#endif

    printk("System RAM: %luMB (%lukB)\n", 
           nr_pages >> (20 - PAGE_SHIFT),
           nr_pages << (PAGE_SHIFT - 10));

    init_frametable();

    end_boot_allocator();

    init_xenheap_pages(__pa(heap_start), xenheap_phys_end);
    printk("Xen heap: %luMB (%lukB)\n",
	   (xenheap_phys_end-__pa(heap_start)) >> 20,
	   (xenheap_phys_end-__pa(heap_start)) >> 10);

    early_boot = 0;

    start_of_day();

    grant_table_init();

    shadow_mode_init();

    /* Create initial domain 0. */
    dom0 = do_createdomain(0, 0);
    if ( dom0 == NULL )
        panic("Error creating domain 0\n");

    set_bit(_DOMF_privileged, &dom0->domain_flags);

    /* Grab the DOM0 command line. */
    cmdline = (char *)(mod[0].string ? __va(mod[0].string) : NULL);
    if ( cmdline != NULL )
    {
        static char dom0_cmdline[256];

        /* Skip past the image name. */
        while ( *cmdline == ' ' ) cmdline++;
        if ( (cmdline = strchr(cmdline, ' ')) != NULL )
            while ( *cmdline == ' ' ) cmdline++;

        /* Copy the command line to a local buffer. */
        strcpy(dom0_cmdline, cmdline);
        cmdline = dom0_cmdline;

        /* Append any extra parameters. */
        if ( skip_ioapic_setup && !strstr(cmdline, "noapic") )
            strcat(cmdline, " noapic");
        if ( acpi_skip_timer_override &&
             !strstr(cmdline, "acpi_skip_timer_override") )
            strcat(cmdline, " acpi_skip_timer_override");
        if ( (strlen(acpi_param) != 0) && !strstr(cmdline, "acpi=") )
        {
            strcat(cmdline, " acpi=");
            strcat(cmdline, acpi_param);
        }
        if ( !strstr(cmdline, "apic=") )
        {
            if ( apic_verbosity == APIC_VERBOSE )
                strcat(cmdline, " apic=verbose");
            else if ( apic_verbosity == APIC_DEBUG )
                strcat(cmdline, " apic=debug");
        }
    }

    /*
     * We're going to setup domain0 using the module(s) that we stashed safely
     * above our heap. The second module, if present, is an initrd ramdisk.
     */
    if ( construct_dom0(dom0,
                        initial_images_start, 
                        mod[0].mod_end-mod[0].mod_start,
                        (mbi->mods_count == 1) ? 0 :
                        initial_images_start + 
                        (mod[1].mod_start-mod[0].mod_start),
                        (mbi->mods_count == 1) ? 0 :
                        mod[mbi->mods_count-1].mod_end - mod[1].mod_start,
                        cmdline) != 0)
        panic("Could not set up DOM0 guest OS\n");

    /* Scrub RAM that is still free and so may go to an unprivileged domain. */
    scrub_heap_pages();

    init_trace_bufs();

    /* Give up the VGA console if DOM0 is configured to grab it. */
    console_endboot(cmdline && strstr(cmdline, "tty0"));

    /* Hide UART from DOM0 if we're using it */
    serial_endboot();

    domain_unpause_by_systemcontroller(current->domain);
    domain_unpause_by_systemcontroller(dom0);
    startup_cpu_idle_loop();
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
