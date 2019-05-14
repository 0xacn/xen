/******************************************************************************
 * arch/x86/spec_ctrl.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (c) 2017-2018 Citrix Systems Ltd.
 */
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/lib.h>

#include <asm/microcode.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/setup.h>
#include <asm/spec_ctrl.h>
#include <asm/spec_ctrl_asm.h>

/* Cmdline controls for Xen's alternative blocks. */
static bool_t __initdata opt_msr_sc_pv = 1;
static bool_t __initdata opt_msr_sc_hvm = 1;
static bool_t __initdata opt_rsb_pv = 1;
static bool_t __initdata opt_rsb_hvm = 1;
static int8_t __initdata opt_md_clear_pv = -1;
static int8_t __initdata opt_md_clear_hvm = -1;

/* Cmdline controls for Xen's speculative settings. */
static enum ind_thunk {
    THUNK_DEFAULT, /* Decide which thunk to use at boot time. */
    THUNK_NONE,    /* Missing compiler support for thunks. */

    THUNK_RETPOLINE,
    THUNK_LFENCE,
    THUNK_JMP,
} opt_thunk __initdata = THUNK_DEFAULT;
static int8_t __initdata opt_ibrs = -1;
bool_t __read_mostly opt_ibpb = 1;
bool_t __read_mostly opt_ssbd = 0;
int8_t __read_mostly opt_eager_fpu = -1;
int8_t __read_mostly opt_l1d_flush = -1;

bool_t __initdata bsp_delay_spec_ctrl;
uint8_t __read_mostly default_xen_spec_ctrl;
uint8_t __read_mostly default_spec_ctrl_flags;

paddr_t __read_mostly l1tf_addr_mask, __read_mostly l1tf_safe_maddr;
static bool_t __initdata cpu_has_bug_l1tf;
static unsigned int __initdata l1d_maxphysaddr;

static bool_t __initdata cpu_has_bug_msbds_only; /* => minimal HT impact. */
static bool_t __initdata cpu_has_bug_mds; /* Any other M{LP,SB,FB}DS combination. */

static int __init parse_bti(const char *s)
{
    const char *ss;
    int val, rc = 0;

    do {
        ss = strchr(s, ',');
        if ( !ss )
            ss = strchr(s, '\0');

        if ( !strncmp(s, "thunk=", 6) )
        {
            s += 6;

            if ( !strncmp(s, "retpoline", ss - s) )
                opt_thunk = THUNK_RETPOLINE;
            else if ( !strncmp(s, "lfence", ss - s) )
                opt_thunk = THUNK_LFENCE;
            else if ( !strncmp(s, "jmp", ss - s) )
                opt_thunk = THUNK_JMP;
            else
                rc = -EINVAL;
        }
        else if ( (val = parse_boolean("ibrs", s, ss)) >= 0 )
            opt_ibrs = val;
        else if ( (val = parse_boolean("ibpb", s, ss)) >= 0 )
            opt_ibpb = val;
        else if ( (val = parse_boolean("rsb_native", s, ss)) >= 0 )
            opt_rsb_pv = val;
        else if ( (val = parse_boolean("rsb_vmexit", s, ss)) >= 0 )
            opt_rsb_hvm = val;
        else
            rc = -EINVAL;

        s = ss + 1;
    } while ( *ss );

    return rc;
}
custom_param("bti", parse_bti);

static int __init parse_spec_ctrl(char *s)
{
    char *ss;
    int val, rc = 0;

    do {
        ss = strchr(s, ',');
        if ( ss )
            *ss = '\0';

        /* Global and Xen-wide disable. */
        val = parse_bool(s);
        if ( !val )
        {
            opt_msr_sc_pv = 0;
            opt_msr_sc_hvm = 0;

            opt_eager_fpu = 0;

            if ( opt_xpti < 0 )
                opt_xpti = 0;

            if ( opt_smt < 0 )
                opt_smt = 1;

            if ( opt_pv_l1tf < 0 )
                opt_pv_l1tf = 0;

        disable_common:
            opt_rsb_pv = 0;
            opt_rsb_hvm = 0;
            opt_md_clear_pv = 0;
            opt_md_clear_hvm = 0;

            opt_thunk = THUNK_JMP;
            opt_ibrs = 0;
            opt_ibpb = 0;
            opt_ssbd = 0;
            opt_l1d_flush = 0;
        }
        else if ( val > 0 )
            rc = -EINVAL;
        else if ( (val = parse_boolean("xen", s, ss)) >= 0 )
        {
            if ( !val )
                goto disable_common;

            rc = -EINVAL;
        }

        /* Xen's alternative blocks. */
        else if ( (val = parse_boolean("pv", s, ss)) >= 0 )
        {
            opt_msr_sc_pv = val;
            opt_rsb_pv = val;
            opt_md_clear_pv = val;
        }
        else if ( (val = parse_boolean("hvm", s, ss)) >= 0 )
        {
            opt_msr_sc_hvm = val;
            opt_rsb_hvm = val;
            opt_md_clear_hvm = val;
        }
        else if ( (val = parse_boolean("msr-sc", s, ss)) >= 0 )
        {
            opt_msr_sc_pv = val;
            opt_msr_sc_hvm = val;
        }
        else if ( (val = parse_boolean("rsb", s, ss)) >= 0 )
        {
            opt_rsb_pv = val;
            opt_rsb_hvm = val;
        }
        else if ( (val = parse_boolean("md-clear", s, ss)) >= 0 ||
                  (val = parse_boolean("mds", s, ss)) >= 0 )
        {
            opt_md_clear_pv = val;
            opt_md_clear_hvm = val;
        }

        /* Xen's speculative sidechannel mitigation settings. */
        else if ( !strncmp(s, "bti-thunk=", 10) )
        {
            s += 10;

            if ( !strcmp(s, "retpoline") )
                opt_thunk = THUNK_RETPOLINE;
            else if ( !strcmp(s, "lfence") )
                opt_thunk = THUNK_LFENCE;
            else if ( !strcmp(s, "jmp") )
                opt_thunk = THUNK_JMP;
            else
                rc = -EINVAL;
        }
        else if ( (val = parse_boolean("ibrs", s, ss)) >= 0 )
            opt_ibrs = val;
        else if ( (val = parse_boolean("ibpb", s, ss)) >= 0 )
            opt_ibpb = val;
        else if ( (val = parse_boolean("ssbd", s, ss)) >= 0 )
            opt_ssbd = val;
        else if ( (val = parse_boolean("eager-fpu", s, ss)) >= 0 )
            opt_eager_fpu = val;
        else if ( (val = parse_boolean("l1d-flush", s, ss)) >= 0 )
            opt_l1d_flush = val;
        else
            rc = -EINVAL;

        s = ss + 1;
    } while ( ss );

    return rc;
}
custom_param("spec-ctrl", parse_spec_ctrl);

int8_t __read_mostly opt_xpti = -1;

static __init void xpti_init_default(uint64_t caps)
{
    if ( boot_cpu_data.x86_vendor == X86_VENDOR_AMD )
        caps = ARCH_CAPABILITIES_RDCL_NO;

    if ( caps & ARCH_CAPABILITIES_RDCL_NO )
        opt_xpti = 0;
    else
        opt_xpti = OPT_XPTI_DOM0 | OPT_XPTI_DOMU;
}

static __init int parse_xpti(char *s)
{
    char *ss;
    int val, rc = 0;

    /* Inhibit the defaults as an explicit choice has been given. */
    if ( opt_xpti == -1 )
        opt_xpti = 0;

    /* Interpret 'xpti' alone in its positive boolean form. */
    if ( *s == '\0' )
        opt_xpti = OPT_XPTI_DOM0 | OPT_XPTI_DOMU;

    do {
        ss = strchr(s, ',');
        if ( ss )
            *ss = '\0';

        switch ( parse_bool(s) )
        {
        case 0:
            opt_xpti = 0;
            break;

        case 1:
            opt_xpti = OPT_XPTI_DOM0 | OPT_XPTI_DOMU;
            break;

        default:
            if ( !strcmp(s, "default") )
                opt_xpti = -1;
            else if ( (val = parse_boolean("dom0", s, ss)) >= 0 )
                opt_xpti = (opt_xpti & ~OPT_XPTI_DOM0) |
                           (val ? OPT_XPTI_DOM0 : 0);
            else if ( (val = parse_boolean("domu", s, ss)) >= 0 )
                opt_xpti = (opt_xpti & ~OPT_XPTI_DOMU) |
                           (val ? OPT_XPTI_DOMU : 0);
            else
                rc = -EINVAL;
            break;
        }

        s = ss + 1;
    } while ( ss );

    return rc;
}
custom_param("xpti", parse_xpti);

int8_t __read_mostly opt_pv_l1tf = -1;

static __init int parse_pv_l1tf(char *s)
{
    char *ss;
    int val, rc = 0;

    /* Inhibit the defaults as an explicit choice has been given. */
    if ( opt_pv_l1tf == -1 )
        opt_pv_l1tf = 0;

    /* Interpret 'pv-l1tf' alone in its positive boolean form. */
    if ( *s == '\0' )
        opt_pv_l1tf = OPT_PV_L1TF_DOM0 | OPT_PV_L1TF_DOMU;

    do {
        ss = strchr(s, ',');
        if ( ss )
            *ss = '\0';

        switch ( parse_bool(s) )
        {
        case 0:
            opt_pv_l1tf = 0;
            break;

        case 1:
            opt_pv_l1tf = OPT_PV_L1TF_DOM0 | OPT_PV_L1TF_DOMU;
            break;

        default:
            if ( (val = parse_boolean("dom0", s, ss)) >= 0 )
                opt_pv_l1tf = ((opt_pv_l1tf & ~OPT_PV_L1TF_DOM0) |
                               (val ? OPT_PV_L1TF_DOM0 : 0));
            else if ( (val = parse_boolean("domu", s, ss)) >= 0 )
                opt_pv_l1tf = ((opt_pv_l1tf & ~OPT_PV_L1TF_DOMU) |
                               (val ? OPT_PV_L1TF_DOMU : 0));
            else
                rc = -EINVAL;
            break;
        }

        s = ss + 1;
    } while ( ss );

    return rc;
}
custom_param("pv-l1tf", parse_pv_l1tf);

static void __init print_details(enum ind_thunk thunk, uint64_t caps)
{
    unsigned int _7d0 = 0, e8b = 0, tmp;

    /* Collect diagnostics about available mitigations. */
    if ( boot_cpu_data.cpuid_level >= 7 )
        cpuid_count(7, 0, &tmp, &tmp, &tmp, &_7d0);
    if ( boot_cpu_data.extended_cpuid_level >= 0x80000008 )
        cpuid(0x80000008, &tmp, &e8b, &tmp, &tmp);

    printk("Speculative mitigation facilities:\n");

    /* Hardware features which pertain to speculative mitigations. */
    printk("  Hardware features:%s%s%s%s%s%s%s%s%s%s%s%s\n",
           (_7d0 & cpufeat_mask(X86_FEATURE_IBRSB)) ? " IBRS/IBPB" : "",
           (_7d0 & cpufeat_mask(X86_FEATURE_STIBP)) ? " STIBP"     : "",
           (_7d0 & cpufeat_mask(X86_FEATURE_L1D_FLUSH)) ? " L1D_FLUSH" : "",
           (_7d0 & cpufeat_mask(X86_FEATURE_SSBD))  ? " SSBD"      : "",
           (_7d0 & cpufeat_mask(X86_FEATURE_MD_CLEAR)) ? " MD_CLEAR" : "",
           (e8b  & cpufeat_mask(X86_FEATURE_IBPB))  ? " IBPB"      : "",
           (caps & ARCH_CAPABILITIES_IBRS_ALL)      ? " IBRS_ALL"  : "",
           (caps & ARCH_CAPABILITIES_RDCL_NO)       ? " RDCL_NO"   : "",
           (caps & ARCH_CAPS_RSBA)                  ? " RSBA"      : "",
           (caps & ARCH_CAPS_SKIP_L1DFL)            ? " SKIP_L1DFL": "",
           (caps & ARCH_CAPS_SSB_NO)                ? " SSB_NO"    : "",
           (caps & ARCH_CAPS_MDS_NO)                ? " MDS_NO"    : "");

    /* Compiled-in support which pertains to mitigations. */
    if ( IS_ENABLED(CONFIG_INDIRECT_THUNK) || IS_ENABLED(CONFIG_SHADOW_PAGING) )
        printk("  Compiled-in support:"
#ifdef CONFIG_INDIRECT_THUNK
               " INDIRECT_THUNK"
#endif
#ifdef CONFIG_SHADOW_PAGING
               " SHADOW_PAGING"
#endif
               "\n");

    /* Settings for Xen's protection, irrespective of guests. */
    printk("  Xen settings: BTI-Thunk %s, SPEC_CTRL: %s%s, Other:%s%s%s\n",
           thunk == THUNK_NONE      ? "N/A" :
           thunk == THUNK_RETPOLINE ? "RETPOLINE" :
           thunk == THUNK_LFENCE    ? "LFENCE" :
           thunk == THUNK_JMP       ? "JMP" : "?",
           !boot_cpu_has(X86_FEATURE_IBRSB)          ? "No" :
           (default_xen_spec_ctrl & SPEC_CTRL_IBRS)  ? "IBRS+" :  "IBRS-",
           !boot_cpu_has(X86_FEATURE_SSBD)           ? "" :
           (default_xen_spec_ctrl & SPEC_CTRL_SSBD)  ? " SSBD+" : " SSBD-",
           opt_ibpb                                  ? " IBPB"  : "",
           opt_l1d_flush                             ? " L1D_FLUSH" : "",
           opt_md_clear_pv || opt_md_clear_hvm       ? " VERW"  : "");

    /* L1TF diagnostics, printed if vulnerable or PV shadowing is in use. */
    if ( cpu_has_bug_l1tf || opt_pv_l1tf )
        printk("  L1TF: believed%s vulnerable, maxphysaddr L1D %u, CPUID %u"
               ", Safe address %"PRIx64"\n",
               cpu_has_bug_l1tf ? "" : " not",
               l1d_maxphysaddr, paddr_bits, l1tf_safe_maddr);

    /*
     * Alternatives blocks for protecting against and/or virtualising
     * mitigation support for guests.
     */
    printk("  Support for VMs: PV:%s%s%s%s%s, HVM:%s%s%s%s%s\n",
           (boot_cpu_has(X86_FEATURE_SC_MSR_PV) ||
            boot_cpu_has(X86_FEATURE_SC_RSB_PV) ||
            opt_eager_fpu)                           ? ""               : " None",
           boot_cpu_has(X86_FEATURE_SC_MSR_PV)       ? " MSR_SPEC_CTRL" : "",
           boot_cpu_has(X86_FEATURE_SC_RSB_PV)       ? " RSB"           : "",
           opt_eager_fpu                             ? " EAGER_FPU"     : "",
           boot_cpu_has(X86_FEATURE_MD_CLEAR)        ? " MD_CLEAR"      : "",
           (boot_cpu_has(X86_FEATURE_SC_MSR_HVM) ||
            boot_cpu_has(X86_FEATURE_SC_RSB_HVM) ||
            opt_eager_fpu)                           ? ""               : " None",
           boot_cpu_has(X86_FEATURE_SC_MSR_HVM)      ? " MSR_SPEC_CTRL" : "",
           boot_cpu_has(X86_FEATURE_SC_RSB_HVM)      ? " RSB"           : "",
           opt_eager_fpu                             ? " EAGER_FPU"     : "",
           boot_cpu_has(X86_FEATURE_MD_CLEAR)        ? " MD_CLEAR"      : "");

    printk("  XPTI (64-bit PV only): Dom0 %s, DomU %s\n",
           opt_xpti & OPT_XPTI_DOM0 ? "enabled" : "disabled",
           opt_xpti & OPT_XPTI_DOMU ? "enabled" : "disabled");

    printk("  PV L1TF shadowing: Dom0 %s, DomU %s\n",
           opt_pv_l1tf & OPT_PV_L1TF_DOM0  ? "enabled"  : "disabled",
           opt_pv_l1tf & OPT_PV_L1TF_DOMU  ? "enabled"  : "disabled");
}

static bool_t __init check_smt_enabled(void)
{
    uint64_t val;
    unsigned int cpu;

    /*
     * x86_num_siblings defaults to 1 in the absence of other information, and
     * is adjusted based on other topology information found in CPUID leaves.
     *
     * On AMD hardware, it will be the current SMT configuration.  On Intel
     * hardware, it will represent the maximum capability, rather than the
     * current configuration.
     */
    if ( boot_cpu_data.x86_num_siblings < 2 )
        return 0;

    /*
     * Intel Nehalem and later hardware does have an MSR which reports the
     * current count of cores/threads in the package.
     *
     * At the time of writing, it is almost completely undocumented, so isn't
     * virtualised reliably.
     */
    if ( boot_cpu_data.x86_vendor == X86_VENDOR_INTEL && !cpu_has_hypervisor &&
         !rdmsr_safe(MSR_INTEL_CORE_THREAD_COUNT, val) )
        return (MASK_EXTR(val, MSR_CTC_CORE_MASK) !=
                MASK_EXTR(val, MSR_CTC_THREAD_MASK));

    /*
     * Search over the CPUs reported in the ACPI tables.  Any whose APIC ID
     * has a non-zero thread id component indicates that SMT is active.
     */
    for_each_present_cpu ( cpu )
        if ( x86_cpu_to_apicid[cpu] & (boot_cpu_data.x86_num_siblings - 1) )
            return 1;

    return 0;
}

/* Calculate whether Retpoline is known-safe on this CPU. */
static bool_t __init retpoline_safe(uint64_t caps)
{
    unsigned int ucode_rev = this_cpu(ucode_cpu_info).cpu_sig.rev;

    if ( boot_cpu_data.x86_vendor == X86_VENDOR_AMD )
        return 1;

    if ( boot_cpu_data.x86_vendor != X86_VENDOR_INTEL ||
         boot_cpu_data.x86 != 6 )
        return 0;

    /*
     * RSBA may be set by a hypervisor to indicate that we may move to a
     * processor which isn't retpoline-safe.
     */
    if ( caps & ARCH_CAPS_RSBA )
        return 0;

    switch ( boot_cpu_data.x86_model )
    {
    case 0x17: /* Penryn */
    case 0x1d: /* Dunnington */
    case 0x1e: /* Nehalem */
    case 0x1f: /* Auburndale / Havendale */
    case 0x1a: /* Nehalem EP */
    case 0x2e: /* Nehalem EX */
    case 0x25: /* Westmere */
    case 0x2c: /* Westmere EP */
    case 0x2f: /* Westmere EX */
    case 0x2a: /* SandyBridge */
    case 0x2d: /* SandyBridge EP/EX */
    case 0x3a: /* IvyBridge */
    case 0x3e: /* IvyBridge EP/EX */
    case 0x3c: /* Haswell */
    case 0x3f: /* Haswell EX/EP */
    case 0x45: /* Haswell D */
    case 0x46: /* Haswell H */
        return 1;

        /*
         * Broadwell processors are retpoline-safe after specific microcode
         * versions.
         */
    case 0x3d: /* Broadwell */
        return ucode_rev >= 0x2a;
    case 0x47: /* Broadwell H */
        return ucode_rev >= 0x1d;
    case 0x4f: /* Broadwell EP/EX */
        return ucode_rev >= 0xb000021;
    case 0x56: /* Broadwell D */
        switch ( boot_cpu_data.x86_mask )
        {
        case 2:  return ucode_rev >= 0x15;
        case 3:  return ucode_rev >= 0x7000012;
        case 4:  return ucode_rev >= 0xf000011;
        case 5:  return ucode_rev >= 0xe000009;
        default:
            printk("Unrecognised CPU stepping %#x - assuming not reptpoline safe\n",
                   boot_cpu_data.x86_mask);
            return 0;
        }
        break;

        /*
         * Skylake, Kabylake and Cannonlake processors are not retpoline-safe.
         */
    case 0x4e:
    case 0x55:
    case 0x5e:
    case 0x66:
    case 0x67:
    case 0x8e:
    case 0x9e:
        return 0;

    default:
        printk("Unrecognised CPU model %#x - assuming not reptpoline safe\n",
               boot_cpu_data.x86_model);
        return 0;
    }
}

/* Calculate whether this CPU speculates past #NM */
static bool_t __init should_use_eager_fpu(void)
{
    /*
     * Assume all unrecognised processors are ok.  This is only known to
     * affect Intel Family 6 processors.
     */
    if ( boot_cpu_data.x86_vendor != X86_VENDOR_INTEL ||
         boot_cpu_data.x86 != 6 )
        return 0;

    switch ( boot_cpu_data.x86_model )
    {
        /*
         * Core processors since at least Nehalem are vulnerable.
         */
    case 0x1e: /* Nehalem */
    case 0x1f: /* Auburndale / Havendale */
    case 0x1a: /* Nehalem EP */
    case 0x2e: /* Nehalem EX */
    case 0x25: /* Westmere */
    case 0x2c: /* Westmere EP */
    case 0x2f: /* Westmere EX */
    case 0x2a: /* SandyBridge */
    case 0x2d: /* SandyBridge EP/EX */
    case 0x3a: /* IvyBridge */
    case 0x3e: /* IvyBridge EP/EX */
    case 0x3c: /* Haswell */
    case 0x3f: /* Haswell EX/EP */
    case 0x45: /* Haswell D */
    case 0x46: /* Haswell H */
    case 0x3d: /* Broadwell */
    case 0x47: /* Broadwell H */
    case 0x4f: /* Broadwell EP/EX */
    case 0x56: /* Broadwell D */
    case 0x4e: /* Skylake M */
    case 0x55: /* Skylake X */
    case 0x5e: /* Skylake D */
    case 0x66: /* Cannonlake */
    case 0x67: /* Cannonlake? */
    case 0x8e: /* Kabylake M */
    case 0x9e: /* Kabylake D */
        return 1;

        /*
         * Atom processors are not vulnerable.
         */
    case 0x1c: /* Pineview */
    case 0x26: /* Lincroft */
    case 0x27: /* Penwell */
    case 0x35: /* Cloverview */
    case 0x36: /* Cedarview */
    case 0x37: /* Baytrail / Valleyview (Silvermont) */
    case 0x4d: /* Avaton / Rangely (Silvermont) */
    case 0x4c: /* Cherrytrail / Brasswell */
    case 0x4a: /* Merrifield */
    case 0x5a: /* Moorefield */
    case 0x5c: /* Goldmont */
    case 0x5f: /* Denverton */
    case 0x7a: /* Gemini Lake */
        return 0;

        /*
         * Knights processors are not vulnerable.
         */
    case 0x57: /* Knights Landing */
    case 0x85: /* Knights Mill */
        return 0;

    default:
        printk("Unrecognised CPU model %#x - assuming vulnerable to LazyFPU\n",
               boot_cpu_data.x86_model);
        return 1;
    }
}

/* Calculate whether this CPU is vulnerable to L1TF. */
static __init void l1tf_calculations(uint64_t caps)
{
    bool_t hit_default = 0;

    l1d_maxphysaddr = paddr_bits;

    /* L1TF is only known to affect Intel Family 6 processors at this time. */
    if ( boot_cpu_data.x86_vendor == X86_VENDOR_INTEL &&
         boot_cpu_data.x86 == 6 )
    {
        switch ( boot_cpu_data.x86_model )
        {
            /*
             * Core processors since at least Penryn are vulnerable.
             */
        case 0x17: /* Penryn */
        case 0x1d: /* Dunnington */
            cpu_has_bug_l1tf = 1;
            break;

        case 0x1f: /* Auburndale / Havendale */
        case 0x1e: /* Nehalem */
        case 0x1a: /* Nehalem EP */
        case 0x2e: /* Nehalem EX */
        case 0x25: /* Westmere */
        case 0x2c: /* Westmere EP */
        case 0x2f: /* Westmere EX */
            cpu_has_bug_l1tf = 1;
            l1d_maxphysaddr = 44;
            break;

        case 0x2a: /* SandyBridge */
        case 0x2d: /* SandyBridge EP/EX */
        case 0x3a: /* IvyBridge */
        case 0x3e: /* IvyBridge EP/EX */
        case 0x3c: /* Haswell */
        case 0x3f: /* Haswell EX/EP */
        case 0x45: /* Haswell D */
        case 0x46: /* Haswell H */
        case 0x3d: /* Broadwell */
        case 0x47: /* Broadwell H */
        case 0x4f: /* Broadwell EP/EX */
        case 0x56: /* Broadwell D */
        case 0x4e: /* Skylake M */
        case 0x55: /* Skylake X */
        case 0x5e: /* Skylake D */
        case 0x66: /* Cannonlake */
        case 0x67: /* Cannonlake? */
        case 0x8e: /* Kabylake M */
        case 0x9e: /* Kabylake D */
            cpu_has_bug_l1tf = 1;
            l1d_maxphysaddr = 46;
            break;

            /*
             * Atom processors are not vulnerable.
             */
        case 0x1c: /* Pineview */
        case 0x26: /* Lincroft */
        case 0x27: /* Penwell */
        case 0x35: /* Cloverview */
        case 0x36: /* Cedarview */
        case 0x37: /* Baytrail / Valleyview (Silvermont) */
        case 0x4d: /* Avaton / Rangely (Silvermont) */
        case 0x4c: /* Cherrytrail / Brasswell */
        case 0x4a: /* Merrifield */
        case 0x5a: /* Moorefield */
        case 0x5c: /* Goldmont */
        case 0x5f: /* Denverton */
        case 0x7a: /* Gemini Lake */
            break;

            /*
             * Knights processors are not vulnerable.
             */
        case 0x57: /* Knights Landing */
        case 0x85: /* Knights Mill */
            break;

        default:
            /* Defer printk() until we've accounted for RDCL_NO. */
            hit_default = 1;
            cpu_has_bug_l1tf = 1;
            break;
        }
    }

    /* Any processor advertising RDCL_NO should be not vulnerable to L1TF. */
    if ( caps & ARCH_CAPABILITIES_RDCL_NO )
        cpu_has_bug_l1tf = 0;

    if ( cpu_has_bug_l1tf && hit_default )
        printk("Unrecognised CPU model %#x - assuming vulnerable to L1TF\n",
               boot_cpu_data.x86_model);

    /*
     * L1TF safe address heuristics.  These apply to the real hardware we are
     * running on, and are best-effort-only if Xen is virtualised.
     *
     * The address mask which the L1D cache uses, which might be wider than
     * the CPUID-reported maxphysaddr.
     */
    l1tf_addr_mask = ((1ul << l1d_maxphysaddr) - 1) & PAGE_MASK;

    /*
     * To be safe, l1tf_safe_maddr must be above the highest cacheable entity
     * in system physical address space.  However, to preserve space for
     * paged-out metadata, it should be as low as possible above the highest
     * cacheable address, so as to require fewer high-order bits being set.
     *
     * These heuristics are based on some guesswork to improve the likelihood
     * of safety in the common case, including Linux's L1TF mitigation of
     * inverting all address bits in a non-present PTE.
     *
     * - If L1D is wider than CPUID (Nehalem and later mobile/desktop/low end
     *   server), setting any address bit beyond CPUID maxphysaddr guarantees
     *   to make the PTE safe.  This case doesn't require all the high-order
     *   bits being set, and doesn't require any other source of information
     *   for safety.
     *
     * - If L1D is the same as CPUID (Pre-Nehalem, or high end server), we
     *   must sacrifice high order bits from the real address space for
     *   safety.  Therefore, make a blind guess that there is nothing
     *   cacheable in the top quarter of physical address space.
     *
     *   It is exceedingly unlikely for machines to be populated with this
     *   much RAM (likely 512G on pre-Nehalem, 16T on Nehalem/Westmere, 64T on
     *   Sandybridge and later) due to the sheer volume of DIMMs this would
     *   actually take.
     *
     *   However, it is possible to find machines this large, so the "top
     *   quarter" guess is supplemented to push the limit higher if references
     *   to cacheable mappings (E820/SRAT/EFI/etc) are found above the top
     *   quarter boundary.
     *
     *   Finally, this top quarter guess gives us a good chance of being safe
     *   when running virtualised (and the CPUID maxphysaddr hasn't been
     *   levelled for heterogeneous migration safety), where the safety
     *   consideration is still in terms of host details, but all E820/etc
     *   information is in terms of guest physical layout.
     */
    l1tf_safe_maddr = max(l1tf_safe_maddr, ((l1d_maxphysaddr > paddr_bits)
                                            ? (1ul << paddr_bits)
                                            : (3ul << (paddr_bits - 2))));
}

/* Calculate whether this CPU is vulnerable to MDS. */
static __init void mds_calculations(uint64_t caps)
{
    /* MDS is only known to affect Intel Family 6 processors at this time. */
    if ( boot_cpu_data.x86_vendor != X86_VENDOR_INTEL ||
         boot_cpu_data.x86 != 6 )
        return;

    /* Any processor advertising MDS_NO should be not vulnerable to MDS. */
    if ( caps & ARCH_CAPS_MDS_NO )
        return;

    switch ( boot_cpu_data.x86_model )
    {
        /*
         * Core processors since at least Nehalem are vulnerable.
         */
    case 0x1f: /* Auburndale / Havendale */
    case 0x1e: /* Nehalem */
    case 0x1a: /* Nehalem EP */
    case 0x2e: /* Nehalem EX */
    case 0x25: /* Westmere */
    case 0x2c: /* Westmere EP */
    case 0x2f: /* Westmere EX */
    case 0x2a: /* SandyBridge */
    case 0x2d: /* SandyBridge EP/EX */
    case 0x3a: /* IvyBridge */
    case 0x3e: /* IvyBridge EP/EX */
    case 0x3c: /* Haswell */
    case 0x3f: /* Haswell EX/EP */
    case 0x45: /* Haswell D */
    case 0x46: /* Haswell H */
    case 0x3d: /* Broadwell */
    case 0x47: /* Broadwell H */
    case 0x4f: /* Broadwell EP/EX */
    case 0x56: /* Broadwell D */
    case 0x4e: /* Skylake M */
    case 0x5e: /* Skylake D */
        cpu_has_bug_mds = 1;
        break;

        /*
         * Some Core processors have per-stepping vulnerability.
         */
    case 0x55: /* Skylake-X / Cascade Lake */
        if ( boot_cpu_data.x86_mask <= 5 )
            cpu_has_bug_mds = 1;
        break;

    case 0x8e: /* Kaby / Coffee / Whiskey Lake M */
        if ( boot_cpu_data.x86_mask <= 0xb )
            cpu_has_bug_mds = 1;
        break;

    case 0x9e: /* Kaby / Coffee / Whiskey Lake D */
        if ( boot_cpu_data.x86_mask <= 0xc )
            cpu_has_bug_mds = 1;
        break;

        /*
         * Very old and very new Atom processors are not vulnerable.
         */
    case 0x1c: /* Pineview */
    case 0x26: /* Lincroft */
    case 0x27: /* Penwell */
    case 0x35: /* Cloverview */
    case 0x36: /* Cedarview */
    case 0x7a: /* Goldmont */
        break;

        /*
         * Middling Atom processors are vulnerable to just the Store Buffer
         * aspect.
         */
    case 0x37: /* Baytrail / Valleyview (Silvermont) */
    case 0x4a: /* Merrifield */
    case 0x4c: /* Cherrytrail / Brasswell */
    case 0x4d: /* Avaton / Rangely (Silvermont) */
    case 0x5a: /* Moorefield */
    case 0x5d:
    case 0x65:
    case 0x6e:
    case 0x75:
        /*
         * Knights processors (which are based on the Silvermont/Airmont
         * microarchitecture) are similarly only affected by the Store Buffer
         * aspect.
         */
    case 0x57: /* Knights Landing */
    case 0x85: /* Knights Mill */
        cpu_has_bug_msbds_only = 1;
        break;

    default:
        printk("Unrecognised CPU model %#x - assuming vulnerable to MDS\n",
               boot_cpu_data.x86_model);
        cpu_has_bug_mds = 1;
        break;
    }
}

void __init init_speculation_mitigations(void)
{
    enum ind_thunk thunk = THUNK_DEFAULT;
    bool_t use_spec_ctrl = 0, ibrs = 0, hw_smt_enabled;
    uint64_t caps = 0;

    if ( boot_cpu_has(X86_FEATURE_ARCH_CAPS) )
        rdmsrl(MSR_ARCH_CAPABILITIES, caps);

    hw_smt_enabled = check_smt_enabled();

    /*
     * Has the user specified any custom BTI mitigations?  If so, follow their
     * instructions exactly and disable all heuristics.
     */
    if ( opt_thunk != THUNK_DEFAULT || opt_ibrs != -1 )
    {
        thunk = opt_thunk;
        ibrs  = !!opt_ibrs;
    }
    else
    {
        /*
         * Evaluate the safest Branch Target Injection mitigations to use.
         * First, begin with compiler-aided mitigations.
         */
        if ( IS_ENABLED(CONFIG_INDIRECT_THUNK) )
        {
            /*
             * AMD's recommended mitigation is to set lfence as being dispatch
             * serialising, and to use IND_THUNK_LFENCE.
             */
            if ( cpu_has_lfence_dispatch )
                thunk = THUNK_LFENCE;
            /*
             * On Intel hardware, we'd like to use retpoline in preference to
             * IBRS, but only if it is safe on this hardware.
             */
            else if ( retpoline_safe(caps) )
                thunk = THUNK_RETPOLINE;
            else if ( boot_cpu_has(X86_FEATURE_IBRSB) )
                ibrs = 1;
        }
        /* Without compiler thunk support, use IBRS if available. */
        else if ( boot_cpu_has(X86_FEATURE_IBRSB) )
            ibrs = 1;
    }

    /*
     * Supplimentary minor adjustments.  Without compiler support, there are
     * no thunks.
     */
    if ( !IS_ENABLED(CONFIG_INDIRECT_THUNK) )
        thunk = THUNK_NONE;

    /*
     * If IBRS is in use and thunks are compiled in, there is no point
     * suffering extra overhead.  Switch to the least-overhead thunk.
     */
    if ( ibrs && thunk == THUNK_DEFAULT )
        thunk = THUNK_JMP;

    /*
     * If there are still no thunk preferences, the compiled default is
     * actually retpoline, and it is better than nothing.
     */
    if ( thunk == THUNK_DEFAULT )
        thunk = THUNK_RETPOLINE;

    /* Apply the chosen settings. */
    if ( thunk == THUNK_LFENCE )
        __set_bit(X86_FEATURE_IND_THUNK_LFENCE, boot_cpu_data.x86_capability);
    else if ( thunk == THUNK_JMP )
        __set_bit(X86_FEATURE_IND_THUNK_JMP, boot_cpu_data.x86_capability);

    /*
     * If we are on hardware supporting MSR_SPEC_CTRL, see about setting up
     * the alternatives blocks so we can virtualise support for guests.
     */
    if ( boot_cpu_has(X86_FEATURE_IBRSB) )
    {
        if ( opt_msr_sc_pv )
        {
            use_spec_ctrl = 1;
            __set_bit(X86_FEATURE_SC_MSR_PV, boot_cpu_data.x86_capability);
        }

        if ( opt_msr_sc_hvm )
        {
            use_spec_ctrl = 1;
            __set_bit(X86_FEATURE_SC_MSR_HVM, boot_cpu_data.x86_capability);
        }

        if ( use_spec_ctrl )
            default_spec_ctrl_flags |= SCF_ist_wrmsr;

        if ( ibrs )
            default_xen_spec_ctrl |= SPEC_CTRL_IBRS;
    }

    /* If we have SSBD available, see whether we should use it. */
    if ( boot_cpu_has(X86_FEATURE_SSBD) && opt_ssbd )
        default_xen_spec_ctrl |= SPEC_CTRL_SSBD;

    /*
     * PV guests can poison the RSB to any virtual address from which
     * they can execute a call instruction.  This is necessarily outside
     * of the Xen supervisor mappings.
     *
     * With SMEP enabled, the processor won't speculate into user mappings.
     * Therefore, in this case, we don't need to worry about poisoned entries
     * from 64bit PV guests.
     *
     * 32bit PV guest kernels run in ring 1, so use supervisor mappings.
     * If a processors speculates to 32bit PV guest kernel mappings, it is
     * speculating in 64bit supervisor mode, and can leak data.
     */
    if ( opt_rsb_pv )
    {
        __set_bit(X86_FEATURE_SC_RSB_PV, boot_cpu_data.x86_capability);
        default_spec_ctrl_flags |= SCF_ist_rsb;
    }

    /*
     * HVM guests can always poison the RSB to point at Xen supervisor
     * mappings.
     */
    if ( opt_rsb_hvm )
        __set_bit(X86_FEATURE_SC_RSB_HVM, boot_cpu_data.x86_capability);

    /* Check we have hardware IBPB support before using it... */
    if ( !boot_cpu_has(X86_FEATURE_IBRSB) && !boot_cpu_has(X86_FEATURE_IBPB) )
        opt_ibpb = 0;

    /* Check whether Eager FPU should be enabled by default. */
    if ( opt_eager_fpu == -1 )
        opt_eager_fpu = should_use_eager_fpu();

    /* (Re)init BSP state now that default_spec_ctrl_flags has been calculated. */
    init_shadow_spec_ctrl_state();

    /* If Xen is using any MSR_SPEC_CTRL settings, adjust the idle path. */
    if ( default_xen_spec_ctrl )
        __set_bit(X86_FEATURE_SC_MSR_IDLE, boot_cpu_data.x86_capability);

    if ( opt_xpti == -1 )
        xpti_init_default(caps);

    if ( opt_xpti == 0 )
        __set_bit(X86_FEATURE_NO_XPTI, boot_cpu_data.x86_capability);
    else
        setup_clear_cpu_cap(X86_FEATURE_NO_XPTI);

    l1tf_calculations(caps);

    /*
     * By default, enable PV domU L1TF mitigations on all L1TF-vulnerable
     * hardware.
     */
    if ( opt_pv_l1tf == -1 )
    {
        if ( !cpu_has_bug_l1tf )
            opt_pv_l1tf = 0;
        else
            opt_pv_l1tf = OPT_PV_L1TF_DOMU;
    }

    /*
     * By default, enable L1D_FLUSH on L1TF-vulnerable hardware, unless
     * instructed to skip the flush on vmentry by our outer hypervisor.
     */
    if ( !boot_cpu_has(X86_FEATURE_L1D_FLUSH) )
        opt_l1d_flush = 0;
    else if ( opt_l1d_flush == -1 )
        opt_l1d_flush = cpu_has_bug_l1tf && !(caps & ARCH_CAPS_SKIP_L1DFL);

    /*
     * We do not disable HT by default on affected hardware.
     *
     * Firstly, if the user intends to use exclusively PV, or HVM shadow
     * guests, HT isn't a concern and should remain fully enabled.  Secondly,
     * safety for HVM HAP guests can be arranged by the toolstack with core
     * parking, pinning or cpupool configurations, including mixed setups.
     *
     * However, if we are on affected hardware, with HT enabled, and the user
     * hasn't explicitly chosen whether to use HT or not, nag them to do so.
     */
    if ( opt_smt == -1 && cpu_has_bug_l1tf && hw_smt_enabled )
    {
        printk("******************************************************\n");
        printk("Booted on L1TF-vulnerable hardware with SMT/Hyperthreading\n");
        printk("enabled.  Please assess your configuration and choose an\n");
        printk("explicit 'smt=<bool>' setting.  See XSA-273.\n");
        printk("******************************************************\n");
    }

    mds_calculations(caps);

    /*
     * By default, enable PV and HVM mitigations on MDS-vulnerable hardware.
     * This will only be a token effort for MLPDS/MFBDS when HT is enabled,
     * but it is somewhat better than nothing.
     */
    if ( opt_md_clear_pv == -1 )
        opt_md_clear_pv = ((cpu_has_bug_mds || cpu_has_bug_msbds_only) &&
                           boot_cpu_has(X86_FEATURE_MD_CLEAR));
    if ( opt_md_clear_hvm == -1 )
        opt_md_clear_hvm = ((cpu_has_bug_mds || cpu_has_bug_msbds_only) &&
                            boot_cpu_has(X86_FEATURE_MD_CLEAR));

    /*
     * Enable MDS defences as applicable.  The PV blocks need using all the
     * time, and the Idle blocks need using if either PV or HVM defences are
     * used.
     *
     * HVM is more complicated.  The MD_CLEAR microcode extends L1D_FLUSH with
     * equivelent semantics to avoid needing to perform both flushes on the
     * HVM path.  The HVM blocks don't need activating if our hypervisor told
     * us it was handling L1D_FLUSH, or we are using L1D_FLUSH ourselves.
     */
    if ( opt_md_clear_pv )
        __set_bit(X86_FEATURE_SC_VERW_PV, boot_cpu_data.x86_capability);
    if ( opt_md_clear_pv || opt_md_clear_hvm )
        __set_bit(X86_FEATURE_SC_VERW_IDLE, boot_cpu_data.x86_capability);
    if ( opt_md_clear_hvm && !(caps & ARCH_CAPS_SKIP_L1DFL) && !opt_l1d_flush )
        __set_bit(X86_FEATURE_SC_VERW_HVM, boot_cpu_data.x86_capability);

    /*
     * Warn the user if they are on MLPDS/MFBDS-vulnerable hardware with HT
     * active and no explicit SMT choice.
     */
    if ( opt_smt == -1 && cpu_has_bug_mds && hw_smt_enabled )
    {
        printk("******************************************************\n");
        printk("Booted on MLPDS/MFBDS-vulnerable hardware with SMT/Hyperthreading\n");
        printk("enabled.  Mitigations will not be fully effective.  Please\n");
        printk("choose an explicit smt=<bool> setting.  See XSA-297.\n");
        printk("******************************************************\n");
    }

    print_details(thunk, caps);

    /*
     * If MSR_SPEC_CTRL is available, apply Xen's default setting and discard
     * any firmware settings.  For performance reasons, when safe to do so, we
     * delay applying non-zero settings until after dom0 has been constructed.
     *
     * "when safe to do so" is based on whether we are virtualised.  A native
     * boot won't have any other code running in a position to mount an
     * attack.
     */
    if ( boot_cpu_has(X86_FEATURE_IBRSB) )
    {
        bsp_delay_spec_ctrl = !cpu_has_hypervisor && default_xen_spec_ctrl;

        /*
         * If delaying MSR_SPEC_CTRL setup, use the same mechanism as
         * spec_ctrl_enter_idle(), by using a shadow value of zero.
         */
        if ( bsp_delay_spec_ctrl )
        {
            struct cpu_info *info = get_cpu_info();

            info->shadow_spec_ctrl = 0;
            barrier();
            info->spec_ctrl_flags |= SCF_use_shadow;
            barrier();
        }

        wrmsrl(MSR_SPEC_CTRL, bsp_delay_spec_ctrl ? 0 : default_xen_spec_ctrl);
    }
}

static void __init __maybe_unused build_assertions(void)
{
    /* The optimised assembly relies on this alias. */
    BUILD_BUG_ON(SCF_use_shadow != 1);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
