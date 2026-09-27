/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX)
 *
 * Copyright Microsoft Corp. 2017
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "whp-dispatch.h"
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/address-spaces.h"
#include "exec/exec-all.h"
#include "exec/ioport.h"
#include "exec/ram_addr.h"
#include "exec/memory-remap.h"
#include "qemu-common.h"
#ifndef _MSC_VER
#include "strings.h"
#endif
#include "sysemu/accel.h"
#include "sysemu/whpx.h"
#include "sysemu/sysemu.h"
#include "sysemu/cpus.h"
#include "qemu/abort.h"
#include "qemu/main-loop.h"
#include "hw/boards.h"
#include "hw/i386/apic_internal.h"
#include "hw/i386/apic-msidef.h"
#include "hw/i386/ioapic.h"
#include "hw/pci/msi.h"
#include "qemu/error-report.h"
#include "qemu/queue.h"
#include "qapi/error.h"
#include "migration/migration.h"

#include <sdkddkver.h>
#if !defined(NTDDI_WIN10_CO)
#include "./whpx-headers/WinHvPlatformDefs.h"
#endif
#include <WinHvPlatform.h>
#include <WinHvEmulation.h>

#define WHPX_CPUID_SIGNATURE 0x40000000

struct whpx_state {
    uint64_t mem_quota;
    WHV_PARTITION_HANDLE partition;
};

static const WHV_REGISTER_NAME whpx_register_names[] = {

    /* X64 General purpose registers */
    WHvX64RegisterRax,
    WHvX64RegisterRcx,
    WHvX64RegisterRdx,
    WHvX64RegisterRbx,
    WHvX64RegisterRsp,
    WHvX64RegisterRbp,
    WHvX64RegisterRsi,
    WHvX64RegisterRdi,
    WHvX64RegisterR8,
    WHvX64RegisterR9,
    WHvX64RegisterR10,
    WHvX64RegisterR11,
    WHvX64RegisterR12,
    WHvX64RegisterR13,
    WHvX64RegisterR14,
    WHvX64RegisterR15,
    WHvX64RegisterRip,
    WHvX64RegisterRflags,

    /* X64 Segment registers */
    WHvX64RegisterEs,
    WHvX64RegisterCs,
    WHvX64RegisterSs,
    WHvX64RegisterDs,
    WHvX64RegisterFs,
    WHvX64RegisterGs,
    WHvX64RegisterLdtr,
    WHvX64RegisterTr,

    /* X64 Table registers */
    WHvX64RegisterIdtr,
    WHvX64RegisterGdtr,

    /* X64 Control Registers */
    WHvX64RegisterCr0,
    WHvX64RegisterCr2,
    WHvX64RegisterCr3,
    WHvX64RegisterCr4,
    /* WHvX64RegisterCr8 is at the end, see whpx_register_count() */

    /* X64 Debug Registers */
    /*
     * WHvX64RegisterDr0,
     * WHvX64RegisterDr1,
     * WHvX64RegisterDr2,
     * WHvX64RegisterDr3,
     * WHvX64RegisterDr6,
     * WHvX64RegisterDr7,
     */

    /* X64 Floating Point and Vector Registers */
    WHvX64RegisterXmm0,
    WHvX64RegisterXmm1,
    WHvX64RegisterXmm2,
    WHvX64RegisterXmm3,
    WHvX64RegisterXmm4,
    WHvX64RegisterXmm5,
    WHvX64RegisterXmm6,
    WHvX64RegisterXmm7,
    WHvX64RegisterXmm8,
    WHvX64RegisterXmm9,
    WHvX64RegisterXmm10,
    WHvX64RegisterXmm11,
    WHvX64RegisterXmm12,
    WHvX64RegisterXmm13,
    WHvX64RegisterXmm14,
    WHvX64RegisterXmm15,
    WHvX64RegisterFpMmx0,
    WHvX64RegisterFpMmx1,
    WHvX64RegisterFpMmx2,
    WHvX64RegisterFpMmx3,
    WHvX64RegisterFpMmx4,
    WHvX64RegisterFpMmx5,
    WHvX64RegisterFpMmx6,
    WHvX64RegisterFpMmx7,
    WHvX64RegisterFpControlStatus,
    WHvX64RegisterXmmControlStatus,

    /* X64 MSRs */
    /*
     * WHvX64RegisterTsc is deliberately not part of the bulk register set:
     * writing it back on every runtime sync rewinds each vCPU's TSC by a
     * different amount and leaves the guest with unsynchronized TSCs (Linux
     * then measures a TSC warp and falls back to the HPET, which traps on
     * every read). It is handled separately by whpx_get_tsc/whpx_set_tsc.
     */
    WHvX64RegisterEfer,
#ifdef TARGET_X86_64
    WHvX64RegisterKernelGsBase,
#endif
    WHvX64RegisterApicBase,
    /* WHvX64RegisterPat, */
    WHvX64RegisterSysenterCs,
    WHvX64RegisterSysenterEip,
    WHvX64RegisterSysenterEsp,
    WHvX64RegisterStar,
#ifdef TARGET_X86_64
    WHvX64RegisterLstar,
    WHvX64RegisterCstar,
    WHvX64RegisterSfmask,
#endif

    /* Interrupt / Event Registers */
    /*
     * WHvRegisterPendingInterruption,
     * WHvRegisterInterruptState,
     * WHvRegisterPendingEvent0,
     * WHvRegisterPendingEvent1
     * WHvX64RegisterDeliverabilityNotifications,
     */

    /*
     * Must stay last: when the hypervisor emulates the local APIC, the TPR
     * is part of the APIC state and CR8 is left out of the bulk transfers.
     */
    WHvX64RegisterCr8,
};

struct whpx_register_set {
    WHV_REGISTER_VALUE values[RTL_NUMBER_OF(whpx_register_names)];
};

struct whpx_vcpu {
    WHV_EMULATOR_HANDLE emulator;
    bool window_registered;
    bool interruptable;
    bool ready_for_pic_interrupt;
    uint64_t tpr;
    uint64_t apic_base;
    bool interruption_pending;

    /* Must be the last field as it may have a tail */
    WHV_RUN_VP_EXIT_CONTEXT exit_ctx;
};

static bool whpx_allowed;
static bool whp_dispatch_initialized = false;
static HMODULE hWinHvPlatform, hWinHvEmulation;
static WHV_PROCESSOR_XSAVE_FEATURES whpx_xsave_cap;

struct whpx_state whpx_global;
struct WHPDispatch whp_dispatch;

/* The hypervisor emulates the local APICs (see whpx_init_kernel_irqchip). */
bool whpx_kernel_irqchip;

static UINT32 whpx_register_count(void)
{
    /* WHvX64RegisterCr8 is the last entry of whpx_register_names. */
    return RTL_NUMBER_OF(whpx_register_names) -
           (whpx_apic_in_platform() ? 1 : 0);
}

static bool whpx_has_xsave(void)
{
    return whpx_xsave_cap.XsaveSupport;
}

static bool whpx_has_xsave_supervisor(void)
{
    return whpx_xsave_cap.XsaveSupervisorSupport;
}

/*
 * VP support
 */

static struct whpx_vcpu *get_whpx_vcpu(CPUState *cpu)
{
    return cpu->whpx_vcpu;
}

static WHV_X64_SEGMENT_REGISTER whpx_seg_q2h(const SegmentCache *qs, int v86,
                                             int r86)
{
    WHV_X64_SEGMENT_REGISTER hs;
    unsigned flags = qs->flags;

    hs.Base = qs->base;
    hs.Limit = qs->limit;
    hs.Selector = qs->selector;

    if (v86) {
        hs.Attributes = 0;
        hs.SegmentType = 3;
        hs.Present = 1;
        hs.DescriptorPrivilegeLevel = 3;
        hs.NonSystemSegment = 1;

    } else {
        hs.Attributes = (flags >> DESC_TYPE_SHIFT);

        if (r86) {
            /* hs.Base &= 0xfffff; */
        }
    }

    return hs;
}

static SegmentCache whpx_seg_h2q(const WHV_X64_SEGMENT_REGISTER *hs)
{
    SegmentCache qs;

    qs.base = hs->Base;
    qs.limit = hs->Limit;
    qs.selector = hs->Selector;

    qs.flags = ((uint32_t)hs->Attributes) << DESC_TYPE_SHIFT;

    return qs;
}

/* X64 Extended Control Registers */
static void whpx_set_xcrs_and_xss(CPUState *cpu)
{
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;
    WHV_REGISTER_VALUE reg_val;
    WHV_REGISTER_NAME reg_name;

    if (!whpx_has_xsave()) {
        return;
    }

    /* Only xcr0 is supported by the hypervisor currently */
    reg_val.Reg64 = env->xcr0;
    reg_name = WHvX64RegisterXCr0;
    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index, &reg_name, 1, &reg_val);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to set register xcr0, hr=%08lx", hr);
    }

    /* IA32_XSS */
    if (!whpx_has_xsave_supervisor()) {
        return;
    }

    reg_val.Reg64 = env->xss;
    reg_name = WHvX64RegisterXss;
    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index, &reg_name, 1, &reg_val);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to set register IA32_XSS, hr=%08lx", hr);
    }
}

/* How much vCPU state whpx_set_registers() pushes to the hypervisor. */
enum {
    WHPX_SET_RUNTIME_STATE = 1,
    WHPX_SET_RESET_STATE,
    WHPX_SET_FULL_STATE,
};

static int whpx_get_tsc(CPUState *cpu)
{
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    WHV_REGISTER_NAME tsc_reg = WHvX64RegisterTsc;
    WHV_REGISTER_VALUE tsc_val;
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;

    hr = whp_dispatch.WHvGetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index, &tsc_reg, 1, &tsc_val);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to get TSC, hr=%08lx", hr);
        return -1;
    }

    env->tsc = tsc_val.Reg64;
    return 0;
}

static int whpx_set_tsc(CPUState *cpu)
{
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    WHV_REGISTER_NAME tsc_reg = WHvX64RegisterTsc;
    WHV_REGISTER_VALUE tsc_val;
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;

    /*
     * Suspend the partition prior to setting the TSC to reduce the variance
     * in TSC across vCPUs. When the first vCPU runs post suspend, the
     * partition is automatically resumed.
     */
    if (whp_dispatch.WHvSuspendPartitionTime) {
        /*
         * Unable to suspend partition while setting TSC is not a fatal
         * error. It just increases the likelihood of TSC variance between
         * vCPUs and some guest OS are able to handle that just fine.
         */
        hr = whp_dispatch.WHvSuspendPartitionTime(whpx->partition);
        if (FAILED(hr)) {
            warn_report("WHPX: Failed to suspend partition, hr=%08lx", hr);
        }
    }

    tsc_val.Reg64 = env->tsc;
    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index, &tsc_reg, 1, &tsc_val);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to set TSC, hr=%08lx", hr);
        return -1;
    }

    return 0;
}

/*
 * MAS: local APIC emulated by the hypervisor ("whpx-apic"), ported from
 * upstream QEMU target/i386/whpx/whpx-apic.c (faf20793b5af, QEMU 6.0) with
 * the later upstream fixes that apply to the xAPIC mode used here:
 * TPR is APIC state, not CR8 (4fdbad6c4981); MSIs with vector 0 are dropped
 * (9533cc6d3604); the vCPU is taken out of HLT after a PIC interrupt is
 * queued through WHvRegisterPendingEvent (ca7a6add3bd2, 3588d68bc8da);
 * INIT/SIPI are left to the hypervisor (6ef6a0f04e5e).
 *
 * The PIC and the IOAPIC stay in QEMU. IOAPIC and PCI MSI writes reach the
 * APIC MMIO window of QEMU and are turned into WHvRequestInterrupt();
 * EOIs of level-triggered vectors come back as WHvRunVpExitReasonX64ApicEoi.
 * The guest's own LAPIC accesses (IPIs, EOI, timer) no longer leave the
 * hypervisor.
 */
struct whpx_lapic_state {
    struct {
        uint32_t data;
        uint32_t padding[3];
    } fields[256];
};

/* env->mp_state values, same meaning as KVM_MP_STATE_* */
#define WHPX_MP_STATE_RUNNABLE          0
#define WHPX_MP_STATE_UNINITIALIZED     1
#define WHPX_MP_STATE_INIT_RECEIVED     2
#define WHPX_MP_STATE_HALTED            3

static void whpx_put_apic_state(APICCommonState *s,
                                struct whpx_lapic_state *kapic)
{
    int i;

    memset(kapic, 0, sizeof(*kapic));
    kapic->fields[0x2].data = s->id << 24;
    kapic->fields[0x3].data = s->version | ((APIC_LVT_NB - 1) << 16);
    kapic->fields[0x8].data = s->tpr;
    kapic->fields[0xd].data = s->log_dest << 24;
    kapic->fields[0xe].data = (uint32_t)s->dest_mode << 28 | 0x0fffffff;
    kapic->fields[0xf].data = s->spurious_vec;
    for (i = 0; i < 8; i++) {
        kapic->fields[0x10 + i].data = s->isr[i];
        kapic->fields[0x18 + i].data = s->tmr[i];
        kapic->fields[0x20 + i].data = s->irr[i];
    }

    kapic->fields[0x28].data = s->esr;
    kapic->fields[0x30].data = s->icr[0];
    kapic->fields[0x31].data = s->icr[1];
    for (i = 0; i < APIC_LVT_NB; i++) {
        kapic->fields[0x32 + i].data = s->lvt[i];
    }

    kapic->fields[0x38].data = s->initial_count;
    kapic->fields[0x3e].data = s->divide_conf;
}

static void whpx_get_apic_state(APICCommonState *s,
                                struct whpx_lapic_state *kapic)
{
    int i, v;

    s->id = kapic->fields[0x2].data >> 24;
    s->tpr = kapic->fields[0x8].data;
    s->arb_id = kapic->fields[0x9].data;
    s->log_dest = kapic->fields[0xd].data >> 24;
    s->dest_mode = kapic->fields[0xe].data >> 28;
    s->spurious_vec = kapic->fields[0xf].data;
    for (i = 0; i < 8; i++) {
        s->isr[i] = kapic->fields[0x10 + i].data;
        s->tmr[i] = kapic->fields[0x18 + i].data;
        s->irr[i] = kapic->fields[0x20 + i].data;
    }

    s->esr = kapic->fields[0x28].data;
    s->icr[0] = kapic->fields[0x30].data;
    s->icr[1] = kapic->fields[0x31].data;
    for (i = 0; i < APIC_LVT_NB; i++) {
        s->lvt[i] = kapic->fields[0x32 + i].data;
    }

    s->initial_count = kapic->fields[0x38].data;
    s->divide_conf = kapic->fields[0x3e].data;

    v = (s->divide_conf & 3) | ((s->divide_conf >> 1) & 4);
    s->count_shift = (v + 1) & 7;

    /*
     * The current count is not carried: after a loadvm the timer restarts
     * from the initial count (as with KVM's and AEHD's in-kernel APIC).
     */
    s->initial_count_load_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    apic_next_timer(s, s->initial_count_load_time);
}

static void whpx_apic_put(CPUState *cs, run_on_cpu_data data)
{
    APICCommonState *s = data.host_ptr;
    struct whpx_lapic_state kapic;
    WHV_REGISTER_NAME reg_name = WHvX64RegisterApicBase;
    WHV_REGISTER_VALUE reg_value;
    HRESULT hr;

    memset(&reg_value, 0, sizeof(reg_value));
    reg_value.Reg64 = s->apicbase;
    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
        whpx_global.partition, cs->cpu_index, &reg_name, 1, &reg_value);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to set APIC base, hr=%08lx", hr);
    }

    whpx_put_apic_state(s, &kapic);
    hr = whp_dispatch.WHvSetVirtualProcessorInterruptControllerState2(
        whpx_global.partition, cs->cpu_index, &kapic, sizeof(kapic));
    if (FAILED(hr)) {
        error_report("WHPX: Failed to set local APIC state, hr=%08lx", hr);
        abort();
    }
}

static void whpx_apic_get(CPUState *cs)
{
    APICCommonState *s = APIC_COMMON(X86_CPU(cs)->apic_state);
    struct whpx_lapic_state kapic;
    HRESULT hr;

    hr = whp_dispatch.WHvGetVirtualProcessorInterruptControllerState2(
        whpx_global.partition, cs->cpu_index, &kapic, sizeof(kapic), NULL);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to get local APIC state, hr=%08lx", hr);
        abort();
    }

    whpx_get_apic_state(s, &kapic);
}

/*
 * Activity state (wait-for-SIPI / HLT inside the hypervisor) and the TSC
 * deadline live in the hypervisor with its APIC. Keep them in env->mp_state
 * and env->tsc_deadline, which the "cpu" section already carries.
 */
static void whpx_get_lapic_cpu_state(CPUState *cpu)
{
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    WHV_REGISTER_NAME reg_names[2] = {
        WHvRegisterInternalActivityState, WHvX64RegisterTscDeadline,
    };
    WHV_REGISTER_VALUE reg_values[2];
    HRESULT hr;

    memset(reg_values, 0, sizeof(reg_values));
    hr = whp_dispatch.WHvGetVirtualProcessorRegisters(
        whpx_global.partition, cpu->cpu_index, reg_names, 2, reg_values);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to get APIC activity state, hr=%08lx", hr);
        return;
    }

    if (reg_values[0].InternalActivity.StartupSuspend) {
        env->mp_state = WHPX_MP_STATE_UNINITIALIZED;
    } else if (reg_values[0].InternalActivity.HaltSuspend) {
        env->mp_state = WHPX_MP_STATE_HALTED;
    } else {
        env->mp_state = WHPX_MP_STATE_RUNNABLE;
    }
    env->tsc_deadline = reg_values[1].Reg64;
}

static void whpx_set_lapic_cpu_state(CPUState *cpu, int level)
{
    X86CPU *x86_cpu = X86_CPU(cpu);
    struct CPUX86State *env = &x86_cpu->env;
    WHV_REGISTER_NAME reg_name = WHvRegisterInternalActivityState;
    WHV_REGISTER_VALUE reg_value;
    HRESULT hr;

    /*
     * Reset: application processors wait for INIT/SIPI, which the
     * hypervisor's APIC handles. Full state (loadvm): only the
     * wait-for-SIPI state is put back; a vCPU that was in HLT simply
     * resumes after it, which a guest idle loop tolerates.
     */
    memset(&reg_value, 0, sizeof(reg_value));
    if (level >= WHPX_SET_FULL_STATE) {
        reg_value.InternalActivity.StartupSuspend =
            env->mp_state == WHPX_MP_STATE_UNINITIALIZED ||
            env->mp_state == WHPX_MP_STATE_INIT_RECEIVED;
    } else {
        reg_value.InternalActivity.StartupSuspend = !cpu_is_bsp(x86_cpu);
        env->tsc_deadline = 0;
    }
    env->mp_state = reg_value.InternalActivity.StartupSuspend ?
        WHPX_MP_STATE_UNINITIALIZED : WHPX_MP_STATE_RUNNABLE;

    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
        whpx_global.partition, cpu->cpu_index, &reg_name, 1, &reg_value);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to set APIC activity state, hr=%08lx", hr);
    }

    /*
     * After the TSC (whpx_set_tsc) and the APIC state (post_load). On
     * loadvm also write a zero deadline, so that one armed by the VM that
     * ran before cannot fire into the loaded one; the hypervisor may
     * refuse that while the timer is not in TSC-deadline mode.
     */
    if (env->tsc_deadline || level >= WHPX_SET_FULL_STATE) {
        reg_name = WHvX64RegisterTscDeadline;
        memset(&reg_value, 0, sizeof(reg_value));
        reg_value.Reg64 = env->tsc_deadline;
        hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
            whpx_global.partition, cpu->cpu_index, &reg_name, 1, &reg_value);
        if (FAILED(hr) && env->tsc_deadline) {
            error_report("WHPX: Failed to set TSC deadline, hr=%08lx", hr);
        }
    }
}

/*
 * A PIC interrupt queued through WHvRegisterPendingEvent (unlike
 * WHvRequestInterrupt) does not wake a vCPU halted inside the hypervisor.
 */
static void whpx_vcpu_kick_out_of_hlt(CPUState *cpu)
{
    WHV_REGISTER_NAME reg_name = WHvRegisterInternalActivityState;
    WHV_REGISTER_VALUE reg_value;
    HRESULT hr;

    hr = whp_dispatch.WHvGetVirtualProcessorRegisters(
        whpx_global.partition, cpu->cpu_index, &reg_name, 1, &reg_value);
    if (FAILED(hr) || !reg_value.InternalActivity.HaltSuspend) {
        return;
    }

    reg_value.InternalActivity.HaltSuspend = 0;
    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
        whpx_global.partition, cpu->cpu_index, &reg_name, 1, &reg_value);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to leave HLT, hr=%08lx", hr);
    }
}

static void whpx_request_interrupt(WHV_INTERRUPT_TYPE type, bool logical,
                                   bool level, uint32_t dest, uint32_t vector)
{
    WHV_INTERRUPT_CONTROL interrupt;
    HRESULT hr;

    memset(&interrupt, 0, sizeof(interrupt));
    interrupt.Type = type;
    interrupt.DestinationMode = logical ?
        WHvX64InterruptDestinationModeLogical :
        WHvX64InterruptDestinationModePhysical;
    interrupt.TriggerMode = level ?
        WHvX64InterruptTriggerModeLevel : WHvX64InterruptTriggerModeEdge;
    interrupt.Destination = dest;
    interrupt.Vector = vector;

    hr = whp_dispatch.WHvRequestInterrupt(whpx_global.partition,
                                          &interrupt, sizeof(interrupt));
    if (FAILED(hr)) {
        error_report("WHPX: Failed to request interrupt type %d dest %u "
                     "vector %u, hr=%08lx", (int)type, dest, vector, hr);
    }
}

static void whpx_send_msi(MSIMessage *msg)
{
    uint64_t addr = msg->address;
    uint32_t data = msg->data;
    uint8_t dest = (addr & MSI_ADDR_DEST_ID_MASK) >> MSI_ADDR_DEST_ID_SHIFT;
    uint8_t vector = (data & MSI_DATA_VECTOR_MASK) >> MSI_DATA_VECTOR_SHIFT;
    uint8_t dest_mode = (addr >> MSI_ADDR_DEST_MODE_SHIFT) & 0x1;
    uint8_t trigger_mode = (data >> MSI_DATA_TRIGGER_SHIFT) & 0x1;
    uint8_t delivery = (data >> MSI_DATA_DELIVERY_MODE_SHIFT) & 0x7;

    switch (delivery) {
    case APIC_DM_FIXED:
    case APIC_DM_LOWPRI:
        /* A masked-out / not yet programmed source (upstream 9533cc6d3604) */
        if (vector == 0) {
            return;
        }
        break;
    case APIC_DM_NMI:
    case APIC_DM_INIT:
        break;
    default:
        /* SMI and ExtINT cannot be requested; the QEMU APIC drops them too. */
        return;
    }

    /* WHV_INTERRUPT_TYPE values are the APIC delivery modes. */
    whpx_request_interrupt((WHV_INTERRUPT_TYPE)delivery, dest_mode,
                           trigger_mode, dest, vector);
}

static uint64_t whpx_apic_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    return ~(uint64_t)0;
}

static void whpx_apic_mem_write(void *opaque, hwaddr addr,
                                uint64_t data, unsigned size)
{
    MSIMessage msg = { .address = addr, .data = data };

    whpx_send_msi(&msg);
}

static const MemoryRegionOps whpx_apic_io_ops = {
    .read = whpx_apic_mem_read,
    .write = whpx_apic_mem_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void whpx_apic_set_base(APICCommonState *s, uint64_t val)
{
    s->apicbase = val;
}

/* Same encoding as the other APIC models: the argument is TPR[7:4]. */
static void whpx_apic_set_tpr(APICCommonState *s, uint8_t val)
{
    s->tpr = (val & 0x0f) << 4;
}

static uint8_t whpx_apic_get_tpr(APICCommonState *s)
{
    return s->tpr >> 4;
}

static void whpx_apic_vapic_base_update(APICCommonState *s)
{
    /* No TPR patching: the hypervisor handles TPR accesses itself. */
}

static void whpx_apic_external_nmi(APICCommonState *s)
{
    whpx_request_interrupt(WHvX64InterruptTypeNmi, false, false, s->id, 2);
}

static void whpx_apic_post_load(APICCommonState *s)
{
    run_on_cpu(CPU(s->cpu), whpx_apic_put, RUN_ON_CPU_HOST_PTR(s));
}

static void whpx_apic_reset(APICCommonState *s)
{
    /* Not used: INIT/SIPI are handled by the hypervisor. */
    s->wait_for_sipi = 0;

    run_on_cpu(CPU(s->cpu), whpx_apic_put, RUN_ON_CPU_HOST_PTR(s));
}

static void whpx_apic_realize(DeviceState *dev, Error **errp)
{
    APICCommonState *s = APIC_COMMON(dev);

    memory_region_init_io(&s->io_memory, OBJECT(s), &whpx_apic_io_ops, s,
                          "whpx-apic-msi", APIC_SPACE_SIZE);

    msi_nonbroken = true;
}

static void whpx_apic_unrealize(DeviceState *dev, Error **errp)
{
}

static void whpx_apic_class_init(ObjectClass *klass, void *data)
{
    APICCommonClass *k = APIC_COMMON_CLASS(klass);

    k->realize = whpx_apic_realize;
    k->unrealize = whpx_apic_unrealize;
    k->reset = whpx_apic_reset;
    k->set_base = whpx_apic_set_base;
    k->set_tpr = whpx_apic_set_tpr;
    k->get_tpr = whpx_apic_get_tpr;
    k->post_load = whpx_apic_post_load;
    k->vapic_base_update = whpx_apic_vapic_base_update;
    k->external_nmi = whpx_apic_external_nmi;
    k->send_msi = whpx_send_msi;
}

static const TypeInfo whpx_apic_info = {
    .name = "whpx-apic",
    .parent = TYPE_APIC_COMMON,
    .instance_size = sizeof(APICCommonState),
    .class_init = whpx_apic_class_init,
};

/*
 * MAS: the event the hypervisor is about to deliver
 * (WHvRegisterPendingInterruption) is vCPU state like any register. When
 * whpx_vcpu_pre_run() queues an interrupt, the local APIC has already moved
 * the vector from IRR to ISR, so a snapshot taken before the guest took it
 * keeps that vector in service forever: the vCPU never sees its timer or any
 * other interrupt of equal or lower priority again after loadvm. Keep the
 * event in env->interrupt_injected / env->nmi_injected (part of the "cpu"
 * section) and put it back when the full state is loaded; clear it on reset
 * and on loadvm otherwise, so that an event of the VM that ran before is not
 * delivered into the loaded one.
 */
static void whpx_get_pending_interruption(CPUState *cpu)
{
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    struct whpx_state *whpx = &whpx_global;
    WHV_REGISTER_NAME reg_name = WHvRegisterPendingInterruption;
    WHV_REGISTER_VALUE reg_value;
    HRESULT hr;

    env->interrupt_injected = -1;
    env->nmi_injected = 0;

    hr = whp_dispatch.WHvGetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index, &reg_name, 1, &reg_value);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to get pending interruption, hr=%08lx", hr);
        return;
    }

    if (!reg_value.PendingInterruption.InterruptionPending) {
        return;
    }

    switch (reg_value.PendingInterruption.InterruptionType) {
    case WHvX64PendingInterrupt:
        env->interrupt_injected =
            reg_value.PendingInterruption.InterruptionVector;
        break;
    case WHvX64PendingNmi:
        env->nmi_injected = 1;
        break;
    default:
        /* Pending exceptions are not carried over (not seen in practice). */
        break;
    }
}

static void whpx_set_pending_interruption(CPUState *cpu, bool from_env)
{
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    struct whpx_state *whpx = &whpx_global;
    struct whpx_vcpu *vcpu = get_whpx_vcpu(cpu);
    WHV_REGISTER_NAME reg_name = WHvRegisterPendingInterruption;
    WHV_REGISTER_VALUE reg_value;
    HRESULT hr;

    memset(&reg_value, 0, sizeof(reg_value));

    /*
     * Vectors below 32 are exceptions, never external interrupts. Snapshots
     * written before this change carry 0 (or -1) here, i.e. nothing pending.
     */
    if (from_env && env->nmi_injected) {
        reg_value.PendingInterruption.InterruptionPending = 1;
        reg_value.PendingInterruption.InterruptionType = WHvX64PendingNmi;
        reg_value.PendingInterruption.InterruptionVector = 2;
    } else if (from_env && env->interrupt_injected >= 32 &&
               env->interrupt_injected <= 255) {
        reg_value.PendingInterruption.InterruptionPending = 1;
        reg_value.PendingInterruption.InterruptionType = WHvX64PendingInterrupt;
        reg_value.PendingInterruption.InterruptionVector =
            env->interrupt_injected;
    } else {
        env->interrupt_injected = -1;
        env->nmi_injected = 0;
    }

    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index, &reg_name, 1, &reg_value);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to set pending interruption, hr=%08lx", hr);
        return;
    }

    vcpu->interruption_pending =
        reg_value.PendingInterruption.InterruptionPending;
}

static void whpx_set_registers(CPUState *cpu, int level)
{
    struct whpx_state *whpx = &whpx_global;
    struct whpx_vcpu *vcpu = get_whpx_vcpu(cpu);
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    X86CPU *x86_cpu = X86_CPU(cpu);
    struct whpx_register_set vcxt;
    HRESULT hr;
    int idx;
    int idx_next;
    int i;
    int v86, r86;

    assert(cpu_is_stopped(cpu) || qemu_cpu_is_self(cpu));

    /*
     * The TSC has side effects on the guest (and a stale value rewinds the
     * vCPU's clock), so it is only written on reset / full state updates,
     * never on runtime syncs.
     */
    if (level >= WHPX_SET_RESET_STATE) {
        whpx_set_tsc(cpu);
    }

    memset(&vcxt, 0, sizeof(struct whpx_register_set));

    v86 = (env->eflags & VM_MASK);
    r86 = !(env->cr[0] & CR0_PE_MASK);

    vcpu->tpr = cpu_get_apic_tpr(x86_cpu->apic_state);
    vcpu->apic_base = cpu_get_apic_base(x86_cpu->apic_state);

    idx = 0;

    /* Indexes for first 16 registers match between HV and QEMU definitions */
    idx_next = 16;
    for (idx = 0; idx < CPU_NB_REGS; idx += 1) {
        vcxt.values[idx].Reg64 = (uint64_t)env->regs[idx];
    }
    idx = idx_next;

    /* Same goes for RIP and RFLAGS */
    assert(whpx_register_names[idx] == WHvX64RegisterRip);
    vcxt.values[idx++].Reg64 = env->eip;

    assert(whpx_register_names[idx] == WHvX64RegisterRflags);
    vcxt.values[idx++].Reg64 = env->eflags;

    /* Translate 6+4 segment registers. HV and QEMU order matches  */
    assert(idx == WHvX64RegisterEs);
    for (i = 0; i < 6; i += 1, idx += 1) {
        vcxt.values[idx].Segment = whpx_seg_q2h(&env->segs[i], v86, r86);
    }

    assert(idx == WHvX64RegisterLdtr);
    vcxt.values[idx++].Segment = whpx_seg_q2h(&env->ldt, 0, 0);

    assert(idx == WHvX64RegisterTr);
    vcxt.values[idx++].Segment = whpx_seg_q2h(&env->tr, 0, 0);

    assert(idx == WHvX64RegisterIdtr);
    vcxt.values[idx].Table.Base = env->idt.base;
    vcxt.values[idx].Table.Limit = env->idt.limit;
    idx += 1;

    assert(idx == WHvX64RegisterGdtr);
    vcxt.values[idx].Table.Base = env->gdt.base;
    vcxt.values[idx].Table.Limit = env->gdt.limit;
    idx += 1;

    /* CR0, 2, 3, 4 (CR8 is last) */
    assert(whpx_register_names[idx] == WHvX64RegisterCr0);
    vcxt.values[idx++].Reg64 = env->cr[0];
    assert(whpx_register_names[idx] == WHvX64RegisterCr2);
    vcxt.values[idx++].Reg64 = env->cr[2];
    assert(whpx_register_names[idx] == WHvX64RegisterCr3);
    vcxt.values[idx++].Reg64 = env->cr[3];
    assert(whpx_register_names[idx] == WHvX64RegisterCr4);
    vcxt.values[idx++].Reg64 = env->cr[4];

    /* 8 Debug Registers - Skipped */

    /* Extended control registers */
    whpx_set_xcrs_and_xss(cpu);

    /* 16 XMM registers */
    assert(whpx_register_names[idx] == WHvX64RegisterXmm0);
    idx_next = idx + 16;
    for (i = 0; i < sizeof(env->xmm_regs) / sizeof(ZMMReg); i += 1, idx += 1) {
        vcxt.values[idx].Reg128.Low64 = env->xmm_regs[i].ZMM_Q(0);
        vcxt.values[idx].Reg128.High64 = env->xmm_regs[i].ZMM_Q(1);
    }
    idx = idx_next;

    /* 8 FP registers */
    assert(whpx_register_names[idx] == WHvX64RegisterFpMmx0);
    for (i = 0; i < 8; i += 1, idx += 1) {
        vcxt.values[idx].Fp.AsUINT128.Low64 = env->fpregs[i].mmx.MMX_Q(0);
        /* vcxt.values[idx].Fp.AsUINT128.High64 =
               env->fpregs[i].mmx.MMX_Q(1);
        */
    }

    /* FP control status register */
    assert(whpx_register_names[idx] == WHvX64RegisterFpControlStatus);
    vcxt.values[idx].FpControlStatus.FpControl = env->fpuc;
    vcxt.values[idx].FpControlStatus.FpStatus =
        (env->fpus & ~0x3800) | (env->fpstt & 0x7) << 11;
    vcxt.values[idx].FpControlStatus.FpTag = 0;
    for (i = 0; i < 8; ++i) {
        vcxt.values[idx].FpControlStatus.FpTag |= (!env->fptags[i]) << i;
    }
    vcxt.values[idx].FpControlStatus.Reserved = 0;
    vcxt.values[idx].FpControlStatus.LastFpOp = env->fpop;
    vcxt.values[idx].FpControlStatus.LastFpRip = env->fpip;
    idx += 1;

    /* XMM control status register */
    assert(whpx_register_names[idx] == WHvX64RegisterXmmControlStatus);
    vcxt.values[idx].XmmControlStatus.LastFpRdp = 0;
    vcxt.values[idx].XmmControlStatus.XmmStatusControl = env->mxcsr;
    vcxt.values[idx].XmmControlStatus.XmmStatusControlMask = 0x0000ffff;
    idx += 1;

    /* MSRs */
    assert(whpx_register_names[idx] == WHvX64RegisterEfer);
    vcxt.values[idx++].Reg64 = env->efer;
#ifdef TARGET_X86_64
    assert(whpx_register_names[idx] == WHvX64RegisterKernelGsBase);
    vcxt.values[idx++].Reg64 = env->kernelgsbase;
#endif

    assert(whpx_register_names[idx] == WHvX64RegisterApicBase);
    vcxt.values[idx++].Reg64 = vcpu->apic_base;

    /* WHvX64RegisterPat - Skipped */

    assert(whpx_register_names[idx] == WHvX64RegisterSysenterCs);
    vcxt.values[idx++].Reg64 = env->sysenter_cs;
    assert(whpx_register_names[idx] == WHvX64RegisterSysenterEip);
    vcxt.values[idx++].Reg64 = env->sysenter_eip;
    assert(whpx_register_names[idx] == WHvX64RegisterSysenterEsp);
    vcxt.values[idx++].Reg64 = env->sysenter_esp;
    assert(whpx_register_names[idx] == WHvX64RegisterStar);
    vcxt.values[idx++].Reg64 = env->star;
#ifdef TARGET_X86_64
    assert(whpx_register_names[idx] == WHvX64RegisterLstar);
    vcxt.values[idx++].Reg64 = env->lstar;
    assert(whpx_register_names[idx] == WHvX64RegisterCstar);
    vcxt.values[idx++].Reg64 = env->cstar;
    assert(whpx_register_names[idx] == WHvX64RegisterSfmask);
    vcxt.values[idx++].Reg64 = env->fmask;
#endif

    /* Interrupt / Event Registers - Skipped */

    /* CR8, left out by whpx_register_count() with the hypervisor's APIC */
    assert(whpx_register_names[idx] == WHvX64RegisterCr8);
    vcxt.values[idx++].Reg64 = vcpu->tpr;

    assert(idx == RTL_NUMBER_OF(whpx_register_names));

    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index,
        whpx_register_names,
        whpx_register_count(),
        &vcxt.values[0]);

    if (FAILED(hr)) {
        error_report("WHPX: Failed to set virtual processor context, hr=%08lx",
                     hr);
    }

    if (level >= WHPX_SET_RESET_STATE) {
        whpx_set_pending_interruption(cpu, level >= WHPX_SET_FULL_STATE);
        if (whpx_apic_in_platform()) {
            whpx_set_lapic_cpu_state(cpu, level);
        }
    }

    return;
}

/* X64 Extended Control Registers */
static void whpx_get_xcrs_and_xss(CPUState *cpu)
{
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;
    WHV_REGISTER_VALUE reg_val;
    WHV_REGISTER_NAME reg_name;

    if (!whpx_has_xsave()) {
        return;
    }

    /* Only xcr0 is supported by the hypervisor currently */
    reg_name = WHvX64RegisterXCr0;
    hr = whp_dispatch.WHvGetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index, &reg_name, 1, &reg_val);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to get register xcr0, hr=%08lx", hr);
        return;
    }
    env->xcr0 = reg_val.Reg64;

    /* IA32_XSS */
    if (!whpx_has_xsave_supervisor()) {
        return;
    }

    reg_name = WHvX64RegisterXss;
    hr = whp_dispatch.WHvGetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index, &reg_name, 1, &reg_val);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to get register xcr0, hr=%08lx", hr);
        return;
    }
    env->xss = reg_val.Reg64;
}

static void whpx_get_registers(CPUState *cpu)
{
    struct whpx_state *whpx = &whpx_global;
    struct whpx_vcpu *vcpu = get_whpx_vcpu(cpu);
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    X86CPU *x86_cpu = X86_CPU(cpu);
    struct whpx_register_set vcxt;
    uint64_t tpr, apic_base;
    HRESULT hr;
    int idx;
    int idx_next;
    int i;

    assert(cpu_is_stopped(cpu) || qemu_cpu_is_self(cpu));

    if (!env->tsc_valid) {
        whpx_get_tsc(cpu);
        env->tsc_valid = !runstate_is_running();
    }

    hr = whp_dispatch.WHvGetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index,
        whpx_register_names,
        whpx_register_count(),
        &vcxt.values[0]);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to get virtual processor context, hr=%08lx",
                     hr);
    }

    idx = 0;

    /* Indexes for first 16 registers match between HV and QEMU definitions */
    idx_next = 16;
    for (idx = 0; idx < CPU_NB_REGS; idx += 1) {
        env->regs[idx] = vcxt.values[idx].Reg64;
    }
    idx = idx_next;

    /* Same goes for RIP and RFLAGS */
    assert(whpx_register_names[idx] == WHvX64RegisterRip);
    env->eip = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterRflags);
    env->eflags = vcxt.values[idx++].Reg64;

    /* Translate 6+4 segment registers. HV and QEMU order matches  */
    assert(idx == WHvX64RegisterEs);
    for (i = 0; i < 6; i += 1, idx += 1) {
        env->segs[i] = whpx_seg_h2q(&vcxt.values[idx].Segment);
    }

    assert(idx == WHvX64RegisterLdtr);
    env->ldt = whpx_seg_h2q(&vcxt.values[idx++].Segment);
    assert(idx == WHvX64RegisterTr);
    env->tr = whpx_seg_h2q(&vcxt.values[idx++].Segment);
    assert(idx == WHvX64RegisterIdtr);
    env->idt.base = vcxt.values[idx].Table.Base;
    env->idt.limit = vcxt.values[idx].Table.Limit;
    idx += 1;
    assert(idx == WHvX64RegisterGdtr);
    env->gdt.base = vcxt.values[idx].Table.Base;
    env->gdt.limit = vcxt.values[idx].Table.Limit;
    idx += 1;

    /* CR0, 2, 3, 4 (CR8 is last) */
    assert(whpx_register_names[idx] == WHvX64RegisterCr0);
    env->cr[0] = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterCr2);
    env->cr[2] = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterCr3);
    env->cr[3] = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterCr4);
    env->cr[4] = vcxt.values[idx++].Reg64;

    /* 8 Debug Registers - Skipped */

    /* Extended control registers */
    whpx_get_xcrs_and_xss(cpu);

    /* 16 XMM registers */
    assert(whpx_register_names[idx] == WHvX64RegisterXmm0);
    idx_next = idx + 16;
    for (i = 0; i < sizeof(env->xmm_regs) / sizeof(ZMMReg); i += 1, idx += 1) {
        env->xmm_regs[i].ZMM_Q(0) = vcxt.values[idx].Reg128.Low64;
        env->xmm_regs[i].ZMM_Q(1) = vcxt.values[idx].Reg128.High64;
    }
    idx = idx_next;

    /* 8 FP registers */
    assert(whpx_register_names[idx] == WHvX64RegisterFpMmx0);
    for (i = 0; i < 8; i += 1, idx += 1) {
        env->fpregs[i].mmx.MMX_Q(0) = vcxt.values[idx].Fp.AsUINT128.Low64;
        /* env->fpregs[i].mmx.MMX_Q(1) =
               vcxt.values[idx].Fp.AsUINT128.High64;
        */
    }

    /* FP control status register */
    assert(whpx_register_names[idx] == WHvX64RegisterFpControlStatus);
    env->fpuc = vcxt.values[idx].FpControlStatus.FpControl;
    env->fpstt = (vcxt.values[idx].FpControlStatus.FpStatus >> 11) & 0x7;
    env->fpus = vcxt.values[idx].FpControlStatus.FpStatus & ~0x3800;
    for (i = 0; i < 8; ++i) {
        env->fptags[i] = !((vcxt.values[idx].FpControlStatus.FpTag >> i) & 1);
    }
    env->fpop = vcxt.values[idx].FpControlStatus.LastFpOp;
    env->fpip = vcxt.values[idx].FpControlStatus.LastFpRip;
    idx += 1;

    /* XMM control status register */
    assert(whpx_register_names[idx] == WHvX64RegisterXmmControlStatus);
    env->mxcsr = vcxt.values[idx].XmmControlStatus.XmmStatusControl;
    idx += 1;

    /* MSRs */
    assert(whpx_register_names[idx] == WHvX64RegisterEfer);
    env->efer = vcxt.values[idx++].Reg64;
#ifdef TARGET_X86_64
    assert(whpx_register_names[idx] == WHvX64RegisterKernelGsBase);
    env->kernelgsbase = vcxt.values[idx++].Reg64;
#endif

    assert(whpx_register_names[idx] == WHvX64RegisterApicBase);
    apic_base = vcxt.values[idx++].Reg64;
    if (apic_base != vcpu->apic_base) {
        vcpu->apic_base = apic_base;
        cpu_set_apic_base(x86_cpu->apic_state, vcpu->apic_base);
    }

    /* WHvX64RegisterPat - Skipped */

    assert(whpx_register_names[idx] == WHvX64RegisterSysenterCs);
    env->sysenter_cs = vcxt.values[idx++].Reg64;;
    assert(whpx_register_names[idx] == WHvX64RegisterSysenterEip);
    env->sysenter_eip = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterSysenterEsp);
    env->sysenter_esp = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterStar);
    env->star = vcxt.values[idx++].Reg64;
#ifdef TARGET_X86_64
    assert(whpx_register_names[idx] == WHvX64RegisterLstar);
    env->lstar = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterCstar);
    env->cstar = vcxt.values[idx++].Reg64;
    assert(whpx_register_names[idx] == WHvX64RegisterSfmask);
    env->fmask = vcxt.values[idx++].Reg64;
#endif

    /* Interrupt / Event Registers - Skipped */

    assert(whpx_register_names[idx] == WHvX64RegisterCr8);
    if (!whpx_apic_in_platform()) {
        tpr = vcxt.values[idx].Reg64;
        if (tpr != vcpu->tpr) {
            vcpu->tpr = tpr;
            cpu_set_apic_tpr(x86_cpu->apic_state, tpr);
        }
    }
    idx++;

    assert(idx == RTL_NUMBER_OF(whpx_register_names));

    whpx_get_pending_interruption(cpu);

    if (whpx_apic_in_platform()) {
        /* TPR, IRR/ISR, LVTs, timer: all live in the hypervisor's APIC */
        whpx_apic_get(cpu);
        whpx_get_lapic_cpu_state(cpu);
    }

    return;
}

static HRESULT CALLBACK whpx_emu_ioport_callback(
    void *ctx,
    WHV_EMULATOR_IO_ACCESS_INFO *IoAccess)
{
    MemTxAttrs attrs = { 0 };
    address_space_rw(&address_space_io, IoAccess->Port, attrs,
                     (uint8_t *)&IoAccess->Data, IoAccess->AccessSize,
                     IoAccess->Direction);
    return S_OK;
}

static HRESULT CALLBACK whpx_emu_mmio_callback(
    void *ctx,
    WHV_EMULATOR_MEMORY_ACCESS_INFO *ma)
{
    cpu_physical_memory_rw(ma->GpaAddress, ma->Data, ma->AccessSize,
                           ma->Direction);
    return S_OK;
}

static HRESULT CALLBACK whpx_emu_getreg_callback(
    void *ctx,
    const WHV_REGISTER_NAME *RegisterNames,
    UINT32 RegisterCount,
    WHV_REGISTER_VALUE *RegisterValues)
{
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;
    CPUState *cpu = (CPUState *)ctx;

    hr = whp_dispatch.WHvGetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index,
        RegisterNames, RegisterCount,
        RegisterValues);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to get virtual processor registers,"
                     " hr=%08lx", hr);
    }

    return hr;
}

static HRESULT CALLBACK whpx_emu_setreg_callback(
    void *ctx,
    const WHV_REGISTER_NAME *RegisterNames,
    UINT32 RegisterCount,
    const WHV_REGISTER_VALUE *RegisterValues)
{
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;
    CPUState *cpu = (CPUState *)ctx;

    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
        whpx->partition, cpu->cpu_index,
        RegisterNames, RegisterCount,
        RegisterValues);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to set virtual processor registers,"
                     " hr=%08lx", hr);
    }

    /*
     * The emulator just successfully wrote the register state. We clear the
     * dirty state so we avoid the double write on resume of the VP.
     */
    cpu->vcpu_dirty = false;

    return hr;
}

static HRESULT CALLBACK whpx_emu_translate_callback(
    void *ctx,
    WHV_GUEST_VIRTUAL_ADDRESS Gva,
    WHV_TRANSLATE_GVA_FLAGS TranslateFlags,
    WHV_TRANSLATE_GVA_RESULT_CODE *TranslationResult,
    WHV_GUEST_PHYSICAL_ADDRESS *Gpa)
{
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;
    CPUState *cpu = (CPUState *)ctx;
    WHV_TRANSLATE_GVA_RESULT res;

    hr = whp_dispatch.WHvTranslateGva(whpx->partition, cpu->cpu_index,
                                      Gva, TranslateFlags, &res, Gpa);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to translate GVA, hr=%08lx", hr);
    } else {
        *TranslationResult = res.ResultCode;
    }

    return hr;
}

static const WHV_EMULATOR_CALLBACKS whpx_emu_callbacks = {
    .Size = sizeof(WHV_EMULATOR_CALLBACKS),
    .WHvEmulatorIoPortCallback = whpx_emu_ioport_callback,
    .WHvEmulatorMemoryCallback = whpx_emu_mmio_callback,
    .WHvEmulatorGetVirtualProcessorRegisters = whpx_emu_getreg_callback,
    .WHvEmulatorSetVirtualProcessorRegisters = whpx_emu_setreg_callback,
    .WHvEmulatorTranslateGvaPage = whpx_emu_translate_callback,
};

static int whpx_handle_mmio(CPUState *cpu, WHV_MEMORY_ACCESS_CONTEXT *ctx)
{
    HRESULT hr;
    struct whpx_vcpu *vcpu = get_whpx_vcpu(cpu);
    struct whpx_vcpu vcpu_copy = *vcpu;
    WHV_MEMORY_ACCESS_CONTEXT ctx_copy = *ctx;
    WHV_EMULATOR_STATUS emu_status;

    hr = whp_dispatch.WHvEmulatorTryMmioEmulation(
        vcpu->emulator, cpu,
        &vcpu->exit_ctx.VpContext, ctx,
        &emu_status);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to parse MMIO access, hr=%08lx", hr);
        return -1;
    }

    if (!emu_status.EmulationSuccessful) {
        if (emu_status.GuestCannotBeFaulted) {
            /* Resume the VP. This is a workaround for a WHP bug until the
             * fix is available in the OS.
             */
            printf("WHPX: Ignoring expected 'GuestCannotBeFaulted' error "
                   "while handling MMIO access.\n");
            return 0;
        }

        /* each byte takes 3 characters to print: "XX " */
        char instruction[sizeof(ctx_copy.InstructionBytes) * 3 + 1];
        int i;
        int size;

        error_report("WHPX: Failed to emulate MMIO access with"
                     " EmulatorReturnStatus: %u (%s%s%s%s%s%s%s%s%s%s)",
            emu_status.AsUINT32,
            (emu_status.EmulationSuccessful ? "EmulationSuccessful " : ""),
            (emu_status.InternalEmulationFailure ?
                "InternalEmulationFailure " : ""),
            (emu_status.IoPortCallbackFailed ? "IoPortCallbackFailed " : ""),
            (emu_status.MemoryCallbackFailed ? "MemoryCallbackFailed " : ""),
            (emu_status.TranslateGvaPageCallbackFailed ?
                "TranslateGvaPageCallbackFailed " : ""),
            (emu_status.TranslateGvaPageCallbackGpaIsNotAligned ?
                "TranslateGvaPageCallbackGpaIsNotAligned " : ""),
            (emu_status.GetVirtualProcessorRegistersCallbackFailed ?
                "GetVirtualProcessorRegistersCallbackFailed " : ""),
            (emu_status.SetVirtualProcessorRegistersCallbackFailed ?
                "SetVirtualProcessorRegistersCallbackFailed " : ""),
            (emu_status.InterruptCausedIntercept ?
                "InterruptCausedIntercept " : ""),
            (emu_status.GuestCannotBeFaulted ? "GuestCannotBeFaulted " : ""));

        error_report("whpx_vcpu { emulator=%p, window_registered=%s, "
                     "interruptable=%s, tpr=%llx, apic_base=%llx, "
                     "interruption_pending=%s }",
            vcpu_copy.emulator,
            (vcpu_copy.window_registered ? "true" : "false"),
            (vcpu_copy.interruptable ? "true" : "false"),
            vcpu_copy.tpr,
            vcpu_copy.apic_base,
            (vcpu_copy.interruption_pending ? "true" : "false"));

        size = MIN(ctx_copy.InstructionByteCount,
                   sizeof(ctx_copy.InstructionBytes));
        for (i = 0; i < size; ++i) {
            sprintf(&instruction[i * 3], "%02X ", ctx_copy.InstructionBytes[i]);
        }

        error_report("WHV_MEMORY_ACCESS_CONTEXT { "
                     "Instruction={ size=%u, bytes='%s%s' }, "
                     "AccessInfo={ AccessType=%u, GpaUnmapped=%u, "
                     "GvaValid=%u, AsUINT32=%u }, Gpa=%llx, Gva=%llx }",
            ctx_copy.InstructionByteCount,
            instruction,
            (ctx_copy.InstructionByteCount >
                sizeof(ctx_copy.InstructionBytes) ? "..." : ""),
            ctx_copy.AccessInfo.AccessType,
            ctx_copy.AccessInfo.GpaUnmapped,
            ctx_copy.AccessInfo.GvaValid,
            ctx_copy.AccessInfo.AsUINT32,
            ctx_copy.Gpa,
            ctx_copy.Gva);

        return -1;
    }

    return 0;
}

static int whpx_handle_portio(CPUState *cpu,
                              WHV_X64_IO_PORT_ACCESS_CONTEXT *ctx)
{
    HRESULT hr;
    struct whpx_vcpu *vcpu = get_whpx_vcpu(cpu);
    WHV_EMULATOR_STATUS emu_status;

    hr = whp_dispatch.WHvEmulatorTryIoEmulation(
        vcpu->emulator, cpu,
        &vcpu->exit_ctx.VpContext, ctx,
        &emu_status);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to parse PortIO access, hr=%08lx", hr);
        return -1;
    }

    if (!emu_status.EmulationSuccessful) {
        error_report("WHPX: Failed to emulate PortIO access with"
                     " EmulatorReturnStatus: %u", emu_status.AsUINT32);
        return -1;
    }

    return 0;
}

static int whpx_handle_halt(CPUState *cpu)
{
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    int ret = 0;

    qemu_mutex_lock_iothread();
    if (!((cpu->interrupt_request & CPU_INTERRUPT_HARD) &&
          (env->eflags & IF_MASK)) &&
        !(cpu->interrupt_request & CPU_INTERRUPT_NMI)) {
        cpu->exception_index = EXCP_HLT;
        cpu->halted = true;
        ret = 1;
    }
    qemu_mutex_unlock_iothread();

    return ret;
}

static void whpx_vcpu_pre_run(CPUState *cpu)
{
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;
    struct whpx_vcpu *vcpu = get_whpx_vcpu(cpu);
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    X86CPU *x86_cpu = X86_CPU(cpu);
    int irq;
    uint8_t tpr;
    WHV_X64_PENDING_INTERRUPTION_REGISTER new_int;
    UINT32 reg_count = 0;
    WHV_REGISTER_VALUE reg_values[4];
    WHV_REGISTER_NAME reg_names[4];
    bool leave_hlt = false;

    memset(&new_int, 0, sizeof(new_int));
    memset(reg_values, 0, sizeof(reg_values));

    qemu_mutex_lock_iothread();

    /* Inject NMI */
    if (!vcpu->interruption_pending &&
        cpu->interrupt_request & (CPU_INTERRUPT_NMI | CPU_INTERRUPT_SMI)) {
        if (cpu->interrupt_request & CPU_INTERRUPT_NMI) {
            cpu->interrupt_request &= ~CPU_INTERRUPT_NMI;
            vcpu->interruptable = false;
            new_int.InterruptionType = WHvX64PendingNmi;
            new_int.InterruptionPending = 1;
            new_int.InterruptionVector = 2;
        }
        if (cpu->interrupt_request & CPU_INTERRUPT_SMI) {
            cpu->interrupt_request &= ~CPU_INTERRUPT_SMI;
        }
    }

    /*
     * Force the VCPU out of its inner loop to process any INIT requests or
     * commit pending TPR access.
     */
    if (cpu->interrupt_request & (CPU_INTERRUPT_INIT | CPU_INTERRUPT_TPR)) {
        if ((cpu->interrupt_request & CPU_INTERRUPT_INIT) &&
            !(env->hflags & HF_SMM_MASK)) {
            cpu->exit_request = 1;
        }
        if (cpu->interrupt_request & CPU_INTERRUPT_TPR) {
            cpu->exit_request = 1;
        }
    }

    /* Get pending hard interruption or replay one that was overwritten */
    if (!whpx_apic_in_platform()) {
        if (!vcpu->interruption_pending &&
            vcpu->interruptable && (env->eflags & IF_MASK)) {
            assert(!new_int.InterruptionPending);
            if (cpu->interrupt_request & CPU_INTERRUPT_HARD) {
                cpu->interrupt_request &= ~CPU_INTERRUPT_HARD;
                irq = cpu_get_pic_interrupt(env);
                if (irq >= 0) {
                    new_int.InterruptionType = WHvX64PendingInterrupt;
                    new_int.InterruptionPending = 1;
                    new_int.InterruptionVector = irq;
                }
            }
        }
    } else if (vcpu->ready_for_pic_interrupt &&
               (cpu->interrupt_request & CPU_INTERRUPT_HARD)) {
        /*
         * With the hypervisor's APIC only the PIC raises
         * CPU_INTERRUPT_HARD; hand its vector over as an ExtINT once the
         * interrupt window is open.
         */
        cpu->interrupt_request &= ~CPU_INTERRUPT_HARD;
        irq = cpu_get_pic_interrupt(env);
        if (irq >= 0) {
            reg_values[reg_count].ExtIntEvent.EventPending = 1;
            reg_values[reg_count].ExtIntEvent.EventType =
                WHvX64PendingEventExtInt;
            reg_values[reg_count].ExtIntEvent.Vector = irq;
            reg_names[reg_count] = WHvRegisterPendingEvent;
            reg_count += 1;
            leave_hlt = true;
        }
    }

    /* Setup interrupt state if new one was prepared */
    if (new_int.InterruptionPending) {
        reg_values[reg_count].PendingInterruption = new_int;
        reg_names[reg_count] = WHvRegisterPendingInterruption;
        reg_count += 1;
        leave_hlt = true;
    }

    /* Sync the TPR to the CR8 if was modified during the intercept */
    tpr = cpu_get_apic_tpr(x86_cpu->apic_state);
    if (!whpx_apic_in_platform() && tpr != vcpu->tpr) {
        vcpu->tpr = tpr;
        reg_values[reg_count].Reg64 = tpr;
        cpu->exit_request = 1;
        reg_names[reg_count] = WHvX64RegisterCr8;
        reg_count += 1;
    }

    /* Update the state of the interrupt delivery notification */
    if (!vcpu->window_registered &&
        cpu->interrupt_request & CPU_INTERRUPT_HARD) {
        reg_values[reg_count].DeliverabilityNotifications.InterruptNotification
            = 1;
        vcpu->window_registered = 1;
        reg_names[reg_count] = WHvX64RegisterDeliverabilityNotifications;
        reg_count += 1;
    }

    qemu_mutex_unlock_iothread();
    vcpu->ready_for_pic_interrupt = false;

    if (reg_count) {
        hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
            whpx->partition, cpu->cpu_index,
            reg_names, reg_count, reg_values);
        if (FAILED(hr)) {
            error_report("WHPX: Failed to set interrupt state registers,"
                         " hr=%08lx", hr);
        }
    }

    /* Events queued above do not end a HLT inside the hypervisor. */
    if (leave_hlt && whpx_apic_in_platform()) {
        whpx_vcpu_kick_out_of_hlt(cpu);
    }

    return;
}

static void whpx_vcpu_post_run(CPUState *cpu)
{
    struct whpx_vcpu *vcpu = get_whpx_vcpu(cpu);
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    X86CPU *x86_cpu = X86_CPU(cpu);
    env->eflags = vcpu->exit_ctx.VpContext.Rflags;

    uint64_t tpr = vcpu->exit_ctx.VpContext.Cr8;
    if (!whpx_apic_in_platform() && vcpu->tpr != tpr) {
        vcpu->tpr = tpr;
        qemu_mutex_lock_iothread();
        cpu_set_apic_tpr(x86_cpu->apic_state, vcpu->tpr);
        qemu_mutex_unlock_iothread();
    }

    vcpu->interruption_pending =
        vcpu->exit_ctx.VpContext.ExecutionState.InterruptionPending;

    vcpu->interruptable =
        !vcpu->exit_ctx.VpContext.ExecutionState.InterruptShadow;

    return;
}

static void whpx_vcpu_process_async_events(CPUState *cpu)
{
    struct CPUX86State *env = (CPUArchState *)(cpu->env_ptr);
    X86CPU *x86_cpu = X86_CPU(cpu);
    struct whpx_vcpu *vcpu = get_whpx_vcpu(cpu);

    if ((cpu->interrupt_request & CPU_INTERRUPT_INIT) &&
        !(env->hflags & HF_SMM_MASK)) {

        do_cpu_init(x86_cpu);
        cpu->vcpu_dirty = true;
        vcpu->interruptable = true;
    }

    if (cpu->interrupt_request & CPU_INTERRUPT_POLL) {
        cpu->interrupt_request &= ~CPU_INTERRUPT_POLL;
        apic_poll_irq(x86_cpu->apic_state);
    }

    if (((cpu->interrupt_request & CPU_INTERRUPT_HARD) &&
         (env->eflags & IF_MASK)) ||
        (cpu->interrupt_request & CPU_INTERRUPT_NMI)) {
        cpu->halted = false;
    }

    if (cpu->interrupt_request & CPU_INTERRUPT_SIPI) {
        if (!cpu->vcpu_dirty) {
            whpx_get_registers(cpu);
        }
        do_cpu_sipi(x86_cpu);
    }

    if (cpu->interrupt_request & CPU_INTERRUPT_TPR) {
        cpu->interrupt_request &= ~CPU_INTERRUPT_TPR;
        if (!cpu->vcpu_dirty) {
            whpx_get_registers(cpu);
        }
        apic_handle_tpr_access_report(x86_cpu->apic_state, env->eip,
                                      env->tpr_access_type);
    }

    return;
}

/*
 * MAS: optional VM-exit statistics (ANDROID_EMU_WHPX_EXIT_STATS=1). Every
 * ~5 s prints the number of exits per reason since the last report, with
 * MMIO split by the device it hit (HPET / local APIC / IOAPIC / other), to
 * tell which trap is hot before optimizing it. Off by default: then it costs
 * one predictable branch per exit.
 */
enum {
    WXS_MMIO_HPET, WXS_MMIO_LAPIC, WXS_MMIO_IOAPIC, WXS_MMIO_OTHER,
    WXS_PIO, WXS_HALT, WXS_IRQ_WINDOW, WXS_CANCELED, WXS_MSR, WXS_CPUID,
    WXS_APIC_EOI, WXS_OTHER, WXS_COUNT
};
static const char *const whpx_exit_stat_names[WXS_COUNT] = {
    "mmio-hpet", "mmio-lapic", "mmio-ioapic", "mmio-other",
    "pio", "halt", "irq-window", "canceled", "msr", "cpuid", "apic-eoi",
    "other"
};
static uint64_t whpx_exit_stats[WXS_COUNT];
static uint64_t whpx_exit_stats_last_ms;
static int whpx_exit_stats_on = -1;

static void whpx_exit_stats_account(const WHV_RUN_VP_EXIT_CONTEXT *ctx)
{
    int k;
    uint64_t now, last;

    if (whpx_exit_stats_on < 0) {
        const char *e = getenv("ANDROID_EMU_WHPX_EXIT_STATS");
        atomic_set(&whpx_exit_stats_last_ms, GetTickCount64());
        whpx_exit_stats_on = (e && e[0] == '1');
    }
    if (!whpx_exit_stats_on) {
        return;
    }

    switch (ctx->ExitReason) {
    case WHvRunVpExitReasonMemoryAccess: {
        uint64_t gpa = ctx->MemoryAccess.Gpa;
        if (gpa >= 0xfed00000ULL && gpa < 0xfed00400ULL) {
            k = WXS_MMIO_HPET;
        } else if (gpa >= 0xfee00000ULL && gpa < 0xfee01000ULL) {
            k = WXS_MMIO_LAPIC;
        } else if (gpa >= 0xfec00000ULL && gpa < 0xfec01000ULL) {
            k = WXS_MMIO_IOAPIC;
        } else {
            k = WXS_MMIO_OTHER;
        }
        break;
    }
    case WHvRunVpExitReasonX64IoPortAccess: k = WXS_PIO; break;
    case WHvRunVpExitReasonX64Halt: k = WXS_HALT; break;
    case WHvRunVpExitReasonX64InterruptWindow: k = WXS_IRQ_WINDOW; break;
    case WHvRunVpExitReasonCanceled: k = WXS_CANCELED; break;
    case WHvRunVpExitReasonX64MsrAccess: k = WXS_MSR; break;
    case WHvRunVpExitReasonX64Cpuid: k = WXS_CPUID; break;
    case WHvRunVpExitReasonX64ApicEoi: k = WXS_APIC_EOI; break;
    default: k = WXS_OTHER; break;
    }
    atomic_inc(&whpx_exit_stats[k]);

    now = GetTickCount64();
    last = atomic_read(&whpx_exit_stats_last_ms);
    if (now - last >= 5000 &&
        atomic_cmpxchg(&whpx_exit_stats_last_ms, last, now) == last) {
        char line[512];
        int len = snprintf(line, sizeof(line), "WHPX exits/%.1fs:",
                           (now - last) / 1000.0);
        for (k = 0; k < WXS_COUNT && len < (int)sizeof(line); k++) {
            len += snprintf(line + len, sizeof(line) - len, " %s=%llu",
                            whpx_exit_stat_names[k],
                            (unsigned long long)
                                atomic_xchg(&whpx_exit_stats[k], 0));
        }
        fprintf(stderr, "%s\n", line);
    }
}

static int whpx_vcpu_run(CPUState *cpu)
{
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;
    struct whpx_vcpu *vcpu = get_whpx_vcpu(cpu);
    int ret;

    whpx_vcpu_process_async_events(cpu);
    /* With the hypervisor's APIC, HLT and wait-for-SIPI stay inside it. */
    if (cpu->halted && !whpx_apic_in_platform()) {
        cpu->exception_index = EXCP_HLT;
        atomic_set(&cpu->exit_request, false);
        return 0;
    }

    qemu_mutex_unlock_iothread();
    cpu_exec_start(cpu);

    do {
        if (cpu->vcpu_dirty) {
            whpx_set_registers(cpu, WHPX_SET_RUNTIME_STATE);
            cpu->vcpu_dirty = false;
        }

        whpx_vcpu_pre_run(cpu);

        if (atomic_read(&cpu->exit_request)) {
            whpx_vcpu_kick(cpu);
        }

        hr = whp_dispatch.WHvRunVirtualProcessor(
            whpx->partition, cpu->cpu_index,
            &vcpu->exit_ctx, sizeof(vcpu->exit_ctx));

        if (FAILED(hr)) {
            error_report("WHPX: Failed to exec a virtual processor,"
                         " hr=%08lx", hr);
            ret = -1;
            break;
        }

        whpx_vcpu_post_run(cpu);
        whpx_exit_stats_account(&vcpu->exit_ctx);

        switch (vcpu->exit_ctx.ExitReason) {
        case WHvRunVpExitReasonMemoryAccess:
            ret = whpx_handle_mmio(cpu, &vcpu->exit_ctx.MemoryAccess);
            break;

        case WHvRunVpExitReasonX64IoPortAccess:
            ret = whpx_handle_portio(cpu, &vcpu->exit_ctx.IoPortAccess);
            break;

        case WHvRunVpExitReasonX64InterruptWindow:
            vcpu->ready_for_pic_interrupt = true;
            vcpu->window_registered = 0;
            ret = 0;
            break;

        case WHvRunVpExitReasonX64ApicEoi:
            /* EOI of a level-triggered vector: let the IOAPIC resample. */
            qemu_mutex_lock_iothread();
            ioapic_eoi_broadcast(vcpu->exit_ctx.ApicEoi.InterruptVector);
            qemu_mutex_unlock_iothread();
            ret = 0;
            break;

        case WHvRunVpExitReasonX64Halt:
            ret = whpx_handle_halt(cpu);
            break;

        case WHvRunVpExitReasonCanceled:
            cpu->exception_index = EXCP_INTERRUPT;
            ret = 1;
            break;

        case WHvRunVpExitReasonX64MsrAccess: {
            WHV_REGISTER_VALUE reg_values[3] = {0};
            WHV_REGISTER_NAME reg_names[3];
            UINT32 reg_count;

            reg_names[0] = WHvX64RegisterRip;
            reg_names[1] = WHvX64RegisterRax;
            reg_names[2] = WHvX64RegisterRdx;

            reg_values[0].Reg64 =
                vcpu->exit_ctx.VpContext.Rip +
                vcpu->exit_ctx.VpContext.InstructionLength;

            /*
             * For all unsupported MSR access we:
             *     ignore writes
             *     return 0 on read.
             */
            reg_count = vcpu->exit_ctx.MsrAccess.AccessInfo.IsWrite ?
                        1 : 3;

            hr = whp_dispatch.WHvSetVirtualProcessorRegisters(whpx->partition,
                                                 cpu->cpu_index,
                                                 reg_names, reg_count,
                                                 reg_values);

            if (FAILED(hr)) {
                error_report("WHPX: Failed to set MsrAccess state "
                             " registers, hr=%08lx", hr);
            }
            ret = 0;
            break;
        }
        case WHvRunVpExitReasonX64Cpuid: {
            WHV_REGISTER_VALUE reg_values[5];
            WHV_REGISTER_NAME reg_names[5];
            UINT32 reg_count = 5;
            UINT64 rip, rax, rcx, rdx, rbx;
            UINT32 signature[3] = {0};

            memset(reg_values, 0, sizeof(reg_values));

            rip = vcpu->exit_ctx.VpContext.Rip +
                  vcpu->exit_ctx.VpContext.InstructionLength;
            switch (vcpu->exit_ctx.CpuidAccess.Rax) {
                case 1:
                    rax = vcpu->exit_ctx.CpuidAccess.DefaultResultRax;
                    /* Advertise that we are running on a hypervisor */
                    rcx =
                        vcpu->exit_ctx.CpuidAccess.DefaultResultRcx |
                        CPUID_EXT_HYPERVISOR;
                    /* The hypervisor's APIC runs in xAPIC mode only. */
                    if (whpx_apic_in_platform()) {
                        rcx &= ~CPUID_EXT_X2APIC;
                    }

                    rdx = vcpu->exit_ctx.CpuidAccess.DefaultResultRdx;
                    rbx = vcpu->exit_ctx.CpuidAccess.DefaultResultRbx;
                    break;
                case 7:
                    rax = vcpu->exit_ctx.CpuidAccess.DefaultResultRax;
                    /* Disable SMAP support*/
                    rbx =
                        vcpu->exit_ctx.CpuidAccess.DefaultResultRbx &
                        ~CPUID_7_0_EBX_SMAP;
                    rcx = vcpu->exit_ctx.CpuidAccess.DefaultResultRcx;
                    rdx = vcpu->exit_ctx.CpuidAccess.DefaultResultRdx;
                    break;
                case WHPX_CPUID_SIGNATURE:
                    memcpy(signature, "WHPXWHPXWHPX", 12);
                    rax = vcpu->exit_ctx.CpuidAccess.DefaultResultRax;
                    rbx = signature[0];
                    rcx = signature[1];
                    rdx = signature[2];
                    break;
                case 0x80000001:
                    rax = vcpu->exit_ctx.CpuidAccess.DefaultResultRax;
                    /* Remove any support of OSVW */
                    rcx =
                        vcpu->exit_ctx.CpuidAccess.DefaultResultRcx &
                        ~CPUID_EXT3_OSVW;

                    rdx = vcpu->exit_ctx.CpuidAccess.DefaultResultRdx;
                    rbx = vcpu->exit_ctx.CpuidAccess.DefaultResultRbx;
                    break;
                default:
                    rax = vcpu->exit_ctx.CpuidAccess.DefaultResultRax;
                    rcx = vcpu->exit_ctx.CpuidAccess.DefaultResultRcx;
                    rdx = vcpu->exit_ctx.CpuidAccess.DefaultResultRdx;
                    rbx = vcpu->exit_ctx.CpuidAccess.DefaultResultRbx;
            }

            reg_names[0] = WHvX64RegisterRip;
            reg_names[1] = WHvX64RegisterRax;
            reg_names[2] = WHvX64RegisterRcx;
            reg_names[3] = WHvX64RegisterRdx;
            reg_names[4] = WHvX64RegisterRbx;

            reg_values[0].Reg64 = rip;
            reg_values[1].Reg64 = rax;
            reg_values[2].Reg64 = rcx;
            reg_values[3].Reg64 = rdx;
            reg_values[4].Reg64 = rbx;

            hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
                whpx->partition, cpu->cpu_index,
                reg_names,
                reg_count,
                reg_values);

            if (FAILED(hr)) {
                error_report("WHPX: Failed to set CpuidAccess state registers,"
                             " hr=%08lx", hr);
            }
            ret = 0;
            break;
        }
        case WHvRunVpExitReasonNone:
        case WHvRunVpExitReasonUnrecoverableException:
        case WHvRunVpExitReasonInvalidVpRegisterValue:
        case WHvRunVpExitReasonUnsupportedFeature:
        case WHvRunVpExitReasonException:
        default:
            error_report("WHPX: Unexpected VP exit code %d",
                         vcpu->exit_ctx.ExitReason);
            whpx_get_registers(cpu);
            qemu_mutex_lock_iothread();
            qemu_system_guest_panicked(cpu_get_crash_info(cpu));
            qemu_mutex_unlock_iothread();
            break;
        }

    } while (!ret);

    cpu_exec_end(cpu);
    qemu_mutex_lock_iothread();
    current_cpu = cpu;

    atomic_set(&cpu->exit_request, false);

    return ret < 0;
}

/* The TSC keeps running while the VM runs, so a cached value is only good
 * while it is stopped (e.g. for savevm). */
static void whpx_cpu_update_state(void *opaque, int running, RunState state)
{
    CPUX86State *env = opaque;

    if (running) {
        env->tsc_valid = false;
    }
}

/*
 * The partition's time (the guest TSC) keeps running while the VM is
 * stopped, but QEMU_CLOCK_VIRTUAL, which drives the HPET, the PIT and the
 * emulated local APIC timer, does not. A guest on the TSC clocksource then
 * sees the TSC jump ahead of every other clock by the length of the stop
 * (pause, savevm) and its clocksource watchdog drops the TSC. savevm also
 * reads the TSC only after the RAM has been written, so a snapshot carries
 * a TSC that is ahead of its own virtual clock and loadvm repeats the jump.
 * Stop the partition's time together with the virtual clock; the TSC read
 * for savevm is then the value at the moment the VM stopped.
 */
static void whpx_vm_state_change(void *opaque, int running, RunState state)
{
    struct whpx_state *whpx = &whpx_global;
    HRESULT hr;

    if (!running) {
        if (whp_dispatch.WHvSuspendPartitionTime) {
            hr = whp_dispatch.WHvSuspendPartitionTime(whpx->partition);
            if (FAILED(hr)) {
                warn_report("WHPX: Failed to suspend partition time,"
                            " hr=%08lx", hr);
            }
        }
    } else if (whp_dispatch.WHvResumePartitionTime) {
        /*
         * Time is suspended here: by the handler above, or by
         * whpx_set_tsc() on the initial reset and on loadvm. Without
         * WHvResumePartitionTime the first WHvRunVirtualProcessor call
         * resumes it.
         */
        hr = whp_dispatch.WHvResumePartitionTime(whpx->partition);
        if (FAILED(hr)) {
            warn_report("WHPX: Failed to resume partition time,"
                        " hr=%08lx", hr);
        }
    }
}

static void do_whpx_cpu_synchronize_state(CPUState *cpu, run_on_cpu_data arg)
{
    whpx_get_registers(cpu);
    cpu->vcpu_dirty = true;
}

static void do_whpx_cpu_synchronize_post_reset(CPUState *cpu,
                                               run_on_cpu_data arg)
{
    whpx_set_registers(cpu, WHPX_SET_RESET_STATE);
    cpu->vcpu_dirty = false;
}

static void do_whpx_cpu_synchronize_post_init(CPUState *cpu,
                                              run_on_cpu_data arg)
{
    whpx_set_registers(cpu, WHPX_SET_FULL_STATE);
    cpu->vcpu_dirty = false;
}

static void do_whpx_cpu_synchronize_pre_loadvm(CPUState *cpu,
                                               run_on_cpu_data arg)
{
    cpu->vcpu_dirty = true;
}

/*
 * CPU support.
 */

void whpx_cpu_synchronize_state(CPUState *cpu)
{
    if (!cpu->vcpu_dirty) {
        run_on_cpu(cpu, do_whpx_cpu_synchronize_state, RUN_ON_CPU_NULL);
    }
}

void whpx_cpu_synchronize_post_reset(CPUState *cpu)
{
    run_on_cpu(cpu, do_whpx_cpu_synchronize_post_reset, RUN_ON_CPU_NULL);
}

void whpx_cpu_synchronize_post_init(CPUState *cpu)
{
    run_on_cpu(cpu, do_whpx_cpu_synchronize_post_init, RUN_ON_CPU_NULL);
}

void whpx_cpu_synchronize_pre_loadvm(CPUState *cpu)
{
    run_on_cpu(cpu, do_whpx_cpu_synchronize_pre_loadvm, RUN_ON_CPU_NULL);
}

/*
 * Vcpu support.
 */

// static Error *whpx_migration_blocker;

int whpx_init_vcpu(CPUState *cpu)
{
    HRESULT hr;
    struct whpx_state *whpx = &whpx_global;
    struct whpx_vcpu *vcpu;
    Error *local_error = NULL;

    // We temporarily disable the migration blocker, allowing emulator
    // Quick Boot to perform. This shouldn't be an issue, unless the
    // snapshot image is moved to a different host.
    //
    /* Add migration blockers for all unsupported features of the
     * Windows Hypervisor Platform
     */
    // if (whpx_migration_blocker == NULL) {
    //     error_setg(&whpx_migration_blocker,
    //            "State blocked due to non-migratable CPUID feature support,"
    //            "dirty memory tracking support, and XSAVE/XRSTOR support");

    //     (void)migrate_add_blocker(whpx_migration_blocker, &local_error);
    //     if (local_error) {
    //         error_report_err(local_error);
    //         migrate_del_blocker(whpx_migration_blocker);
    //         error_free(whpx_migration_blocker);
    //         return -EINVAL;
    //     }
    // }

    vcpu = g_malloc0(sizeof(struct whpx_vcpu));

    if (!vcpu) {
        error_report("WHPX: Failed to allocte VCPU context.");
        return -ENOMEM;
    }

    hr = whp_dispatch.WHvEmulatorCreateEmulator(&whpx_emu_callbacks, &vcpu->emulator);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to setup instruction completion support,"
                     " hr=%08lx", hr);
        g_free(vcpu);
        return -EINVAL;
    }

    hr = whp_dispatch.WHvCreateVirtualProcessor(whpx->partition, cpu->cpu_index, 0);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to create a virtual processor,"
                     " hr=%08lx", hr);
        whp_dispatch.WHvEmulatorDestroyEmulator(vcpu->emulator);
        g_free(vcpu);
        return -EINVAL;
    }

    vcpu->interruptable = true;

    cpu->vcpu_dirty = true;
    cpu->whpx_vcpu = vcpu;
    qemu_add_vm_change_state_handler(whpx_cpu_update_state, cpu->env_ptr);

    return 0;
}

int whpx_vcpu_exec(CPUState *cpu)
{
    int ret;
    int fatal;

    for (;;) {
        if (cpu->exception_index >= EXCP_INTERRUPT) {
            ret = cpu->exception_index;
            cpu->exception_index = -1;
            break;
        }

        fatal = whpx_vcpu_run(cpu);

        if (fatal) {
            error_report("WHPX: Failed to exec a virtual processor");
            abort();
        }
    }

    return ret;
}

void whpx_destroy_vcpu(CPUState *cpu)
{
    struct whpx_state *whpx = &whpx_global;
    struct whpx_vcpu *vcpu = get_whpx_vcpu(cpu);

    whp_dispatch.WHvDeleteVirtualProcessor(whpx->partition, cpu->cpu_index);
    whp_dispatch.WHvEmulatorDestroyEmulator(vcpu->emulator);
    g_free(cpu->whpx_vcpu);
    return;
}

void whpx_vcpu_kick(CPUState *cpu)
{
    struct whpx_state *whpx = &whpx_global;
    whp_dispatch.WHvCancelRunVirtualProcessor(whpx->partition, cpu->cpu_index, 0);
}

/*
 * Memory support.
 */

#define WHPX_MAX_SLOTS 64

struct whpx_slot {
    int present;
    uint64_t size;
    uint64_t gpa_start;
    uint64_t gva;
    void* hva;
};

struct whpx_slot whpx_slots[WHPX_MAX_SLOTS];

void* whpx_gpa2hva(uint64_t gpa, bool* found) {
    struct whpx_slot *mslot;
    *found = false;
    uint32_t i;

    for (i = 0; i < WHPX_MAX_SLOTS; i++) {
        mslot = &whpx_slots[i];
        if (!mslot->size) continue;
        if (gpa >= mslot->gpa_start &&
            gpa < mslot->gpa_start + mslot->size) {
            *found = true;
            return (void*)((char*)(mslot->hva) + (gpa - mslot->gpa_start));
        }
    }

    return 0;
}

struct whpx_slot* whpx_next_free_slot() {
    struct whpx_slot *mem = 0;
    int x;

    for (x = 0; x < WHPX_MAX_SLOTS; ++x) {
        mem = whpx_slots + x;
        if (!mem->size) return mem;
    }

    return mem;
}

struct whpx_slot* whpx_find_overlap_slot(uint64_t start, uint64_t end, bool* coincident) {
    struct whpx_slot *slot = 0;
    int x;

    if (coincident) *coincident = false;

    for (x = 0; x < WHPX_MAX_SLOTS; ++x) {
        slot = whpx_slots + x;
        if (slot->size && start < (slot->gpa_start + slot->size) && end > slot->gpa_start) {
            // Is [start, end) describing the same range?
            if ((start == slot->gpa_start) &&
                (end == slot->gpa_start + slot->size)) {
                if (coincident) *coincident = true;
            }
            return slot;
        }
    }

    return slot;
}

static HRESULT whpx_set_ram(
    VOID* SourceAddress,
    WHV_GUEST_PHYSICAL_ADDRESS GuestAddress,
    UINT64 SizeInBytes,
    WHV_MAP_GPA_RANGE_FLAGS Flags,
    int add) {

    struct whpx_state *whpx = &whpx_global;
    HRESULT hr;

    WHV_PARTITION_HANDLE partition = whpx->partition;

    if (add) {
        hr = whp_dispatch.WHvMapGpaRange(partition,
                                         SourceAddress,
                                         GuestAddress,
                                         SizeInBytes,
                                         Flags);
    } else {
        hr = whp_dispatch.WHvUnmapGpaRange(partition,
                                           GuestAddress,
                                           SizeInBytes);
    }

    if (FAILED(hr)) {
        error_report("WHPX: Failed to %s GPA range PA:%p, Size:%p bytes,"
                     " Host:%p, hr=%08lx",
                     (add ? "MAP" : "UNMAP"),
                     (void *)(uintptr_t)GuestAddress, (void *)SizeInBytes, SourceAddress, hr);
        return hr;
    }

    // Success. Update our internal slots.
    {
        // Could have overlapped with existing slot.
        // If so, and not coincident, reset the old mapping.
        bool coincident;
        struct whpx_slot* overlap_slot =
            whpx_find_overlap_slot(
                GuestAddress,
                GuestAddress + SizeInBytes,
                &coincident);

        if (overlap_slot && coincident) {
            if (add) {
                // Nothing to do because the slot overlaps, return.
                return hr;
            } else {
                // Coincident overlap slot; delete
                overlap_slot->size = 0;
                return hr;
            }
        }

        // Delete existing overlap slot, and add the new one to a free slot,
        // if we are adding a new region.
        if (overlap_slot) {
            overlap_slot->size = 0;
        }

        if (add) {
            struct whpx_slot* new_slot = 0;
            new_slot = whpx_next_free_slot();

            if (!new_slot) {
                qemu_abort("%s: no free slots\n", __func__);
            }

            new_slot->size = SizeInBytes;
            new_slot->gpa_start = GuestAddress;
            new_slot->hva = SourceAddress;
        }
    }

    return hr;
}

static void whpx_update_mapping(hwaddr start_pa, ram_addr_t size,
                                void *host_va, int add, int rom,
                                const char *name)
{
    struct whpx_state *whpx = &whpx_global;

    /*
    if (add) {
        printf("WHPX: ADD PA:%p Size:%p, Host:%p, %s, '%s'\n",
               (void*)start_pa, (void*)size, host_va,
               (rom ? "ROM" : "RAM"), name);
    } else {
        printf("WHPX: DEL PA:%p Size:%p, Host:%p,      '%s'\n",
               (void*)start_pa, (void*)size, host_va, name);
    }
    */

    whpx_set_ram(host_va,
                 start_pa,
                 size,
                 (WHvMapGpaRangeFlagRead |
                  WHvMapGpaRangeFlagExecute |
                  (rom ? 0 : WHvMapGpaRangeFlagWrite)),
                 add);
}

static void whpx_process_section(MemoryRegionSection *section, int add)
{
    MemoryRegion *mr = section->mr;
    hwaddr start_pa = section->offset_within_address_space;
    ram_addr_t size = int128_get64(section->size);
    unsigned int delta;
    uint64_t host_va;

    if (!memory_region_is_ram(mr)) {
        return;
    }

    if (memory_region_is_user_backed(mr)) {
        return;
    }

    delta = qemu_real_host_page_size - (start_pa & ~qemu_real_host_page_mask);
    delta &= ~qemu_real_host_page_mask;
    if (delta > size) {
        return;
    }
    start_pa += delta;
    size -= delta;
    size &= qemu_real_host_page_mask;
    if (!size || (start_pa & ~qemu_real_host_page_mask)) {
        return;
    }

    host_va = (uintptr_t)memory_region_get_ram_ptr(mr)
            + section->offset_within_region + delta;

    whpx_update_mapping(start_pa, size, (void *)(uintptr_t)host_va, add,
                        memory_region_is_rom(mr), mr->name);
}

// User backed memory region API
static WHV_MAP_GPA_RANGE_FLAGS
user_backed_flags_to_whpx(int flags) {
    WHV_MAP_GPA_RANGE_FLAGS whpx_flags = 0;
    if (flags & USER_BACKED_RAM_FLAGS_READ) {
        whpx_flags |= WHvMapGpaRangeFlagRead;
    }
    if (flags & USER_BACKED_RAM_FLAGS_WRITE) {
        whpx_flags |= WHvMapGpaRangeFlagWrite;
    }
    if (flags & USER_BACKED_RAM_FLAGS_EXEC) {
        whpx_flags |= WHvMapGpaRangeFlagExecute;
    }
    return whpx_flags;
}

#ifndef ALIGN
#define ALIGN(x, y) (((x)+(y)-1) & ~((y)-1))
#endif

static void whpx_user_backed_ram_map(uint64_t gpa, void* hva, uint64_t size, int flags) {
    if (!hva) {
        return;
    }
    struct whpx_state *whpx = &whpx_global;
    HRESULT hr;

    // Align size to host page size (0x1000 on x86_64)
    size = ALIGN(size, qemu_real_host_page_size);

    hr = whpx_set_ram(hva, gpa, size, user_backed_flags_to_whpx(flags), 1 /* add */);

    if (FAILED(hr)) {
        error_report(
            "WHPX: Failed to map GPA range "
            "[0x%llx 0x%llx] to HVA %p. flags: 0x%x",
            (unsigned long long)gpa,
            (unsigned long long)(gpa + size),
            hva, flags);
    }
}

static void whpx_user_backed_ram_unmap(uint64_t gpa, uint64_t size) {
    struct whpx_state *whpx = &whpx_global;
    HRESULT hr;

    // Align size to host page size (0x1000 on x86_64)
    size = ALIGN(size, qemu_real_host_page_size);

    hr = whpx_set_ram(0, gpa, size, 0 /* no flags */, 0 /* for delete */);

    if (FAILED(hr)) {
        error_report(
            "WHPX: Failed to unmap GPA range "
            "[0x%llx 0x%llx]",
            (unsigned long long)gpa,
            (unsigned long long)(gpa + size));
    }
}

static void whpx_region_add(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    memory_region_ref(section->mr);
    whpx_process_section(section, 1);
}

static void whpx_region_del(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    whpx_process_section(section, 0);
    memory_region_unref(section->mr);
}

static void whpx_transaction_begin(MemoryListener *listener)
{
}

static void whpx_transaction_commit(MemoryListener *listener)
{
}

static void whpx_log_sync(MemoryListener *listener,
                         MemoryRegionSection *section)
{
    MemoryRegion *mr = section->mr;

    if (!memory_region_is_ram(mr)) {
        return;
    }

    memory_region_set_dirty(mr, 0, int128_get64(section->size));
}

static MemoryListener whpx_memory_listener = {
    .begin = whpx_transaction_begin,
    .commit = whpx_transaction_commit,
    .region_add = whpx_region_add,
    .region_del = whpx_region_del,
    .log_sync = whpx_log_sync,
    .priority = 10,
};

static void whpx_memory_init(void)
{
    memory_listener_register(&whpx_memory_listener, &address_space_memory);
    qemu_set_user_backed_mapping_funcs(
        whpx_user_backed_ram_map,
        whpx_user_backed_ram_unmap);
}

static void whpx_handle_interrupt(CPUState *cpu, int mask)
{
    cpu->interrupt_request |= mask;

    if (!qemu_cpu_is_self(cpu)) {
        qemu_cpu_kick(cpu);
    }
}

/*
 * Partition support
 */
static void whpx_disable_xsave(struct whpx_state *whpx)
{
    WHV_PROCESSOR_XSAVE_FEATURES xsave_cap;
    HRESULT hr;

    typedef int(CALLBACK* PFN_RTLGETVERSION) (PRTL_OSVERSIONINFOW lpVersionInformation);
    PFN_RTLGETVERSION pRtlGetVersion;
    HMODULE hDll = LoadLibrary("Ntdll.dll");
    pRtlGetVersion = (PFN_RTLGETVERSION)GetProcAddress(hDll, "RtlGetVersion");
    if (pRtlGetVersion) {
        RTL_OSVERSIONINFOW ovi = { 0 };
        ovi.dwOSVersionInfoSize = sizeof(ovi);
        int ntStatus = pRtlGetVersion(&ovi);
        // On Windows 11 (10.0.22000), CET will require xsave
        if (ntStatus == 0) {
            printf("WHPX on Windows %d.%d.%d detected.\n",
                   ovi.dwMajorVersion, ovi.dwMinorVersion, ovi.dwBuildNumber);
            if(ovi.dwMajorVersion >= 10 && ovi.dwBuildNumber >= 22000)
                return;
        }
    }

    /*
     * MAS: opt-in to keep XSAVE (and with it AVX/AVX2 in the guest) on
     * Windows 10 as well. Windows 11 already keeps it with this same code;
     * what is missing is only the XSAVE state in snapshots, which we do not
     * use. Unverified on Windows 10 hosts, hence opt-in.
     */
    {
        const char *keep = getenv("ANDROID_EMU_WHPX_KEEP_XSAVE");
        if (keep && keep[0] == '1') {
            printf("WHPX: keeping XSAVE enabled (ANDROID_EMU_WHPX_KEEP_XSAVE)\n");
            return;
        }
    }

    if (!whpx_has_xsave()) {
        return;
    }

    xsave_cap.AsUINT64 = 0;
    hr = whp_dispatch.WHvSetPartitionProperty(
        whpx->partition,
        WHvPartitionPropertyCodeProcessorXsaveFeatures,
        &xsave_cap,
        sizeof(xsave_cap));

    if (FAILED(hr)) {
        return;
    }

    whpx_xsave_cap.AsUINT64 = 0;
}

/*
 * MAS: let the hypervisor emulate the local APICs, so that IPIs, EOIs and
 * the APIC timer no longer exit to QEMU. Opt-in until validated:
 *   ANDROID_EMU_WHPX_KERNEL_IRQCHIP=1  on, where the host supports it; kept
 *                                      off on Windows 10 (upstream QEMU
 *                                      3b124791bdce does the same with a PIC)
 *   ANDROID_EMU_WHPX_KERNEL_IRQCHIP=2  on, Windows 10 included
 *   -machine kernel-irqchip=on         required: fail if unavailable
 *   -machine kernel-irqchip=off        off, whatever the variable says
 * Must run before WHvSetupPartition().
 */
static int whpx_init_kernel_irqchip(MachineState *ms, struct whpx_state *whpx)
{
    const char *env = getenv("ANDROID_EMU_WHPX_KERNEL_IRQCHIP");
    bool required = machine_kernel_irqchip_required(ms);
    bool wanted = required || (env && (env[0] == '1' || env[0] == '2'));
    bool any_windows = required || (env && env[0] == '2');
    WHV_CAPABILITY_FEATURES features;
    WHV_PROCESSOR_PERFMON_FEATURES perfmon;
    WHV_X64_LOCAL_APIC_EMULATION_MODE mode =
        WHvX64LocalApicEmulationModeXApic;
    UINT32 size;
    HRESULT hr;

    whpx_kernel_irqchip = false;

    if (!machine_kernel_irqchip_allowed(ms) || !wanted) {
        return 0;
    }

    memset(&features, 0, sizeof(features));
    hr = whp_dispatch.WHvGetCapability(WHvCapabilityCodeFeatures, &features,
                                       sizeof(features), &size);
    if (FAILED(hr) || !features.LocalApicEmulation ||
        !whp_dispatch.WHvRequestInterrupt ||
        !whp_dispatch.WHvGetVirtualProcessorInterruptControllerState2 ||
        !whp_dispatch.WHvSetVirtualProcessorInterruptControllerState2) {
        error_report("WHPX: APIC emulation in the hypervisor is not available"
                     " on this Windows build (hr=%08lx)", hr);
        return required ? -EINVAL : 0;
    }

    /*
     * Same test as upstream QEMU (68438cb3cbcb): the perfmon capability
     * exists from Windows Server 2022 / Windows 11 on.
     */
    if (!any_windows) {
        memset(&perfmon, 0, sizeof(perfmon));
        hr = whp_dispatch.WHvGetCapability(
            WHvCapabilityCodeProcessorPerfmonFeatures, &perfmon,
            sizeof(perfmon), &size);
        if (FAILED(hr)) {
            printf("WHPX: keeping the local APIC in QEMU on this Windows"
                   " build; ANDROID_EMU_WHPX_KERNEL_IRQCHIP=2 forces it\n");
            return 0;
        }
    }

    hr = whp_dispatch.WHvSetPartitionProperty(
        whpx->partition, WHvPartitionPropertyCodeLocalApicEmulationMode,
        &mode, sizeof(mode));
    if (FAILED(hr)) {
        error_report("WHPX: Failed to enable APIC emulation in the"
                     " hypervisor, hr=%08lx", hr);
        return required ? -EINVAL : 0;
    }

    whpx_kernel_irqchip = true;
    printf("WHPX: local APIC emulated by the hypervisor (xAPIC)\n");
    return 0;
}

static int whpx_accel_init(MachineState *ms)
{
    struct whpx_state *whpx;
    int ret;
    HRESULT hr;
    WHV_CAPABILITY whpx_cap;
    UINT32 whpx_cap_size;
    WHV_PARTITION_PROPERTY prop;

    whpx = &whpx_global;

    if (!init_whp_dispatch()){
        ret = -ENOSYS;
        goto error;
    }

    memset(whpx, 0, sizeof(struct whpx_state));
    memset(whpx_slots, 0, sizeof(whpx_slots));

    whpx->mem_quota = ms->ram_size;

    hr = whp_dispatch.WHvGetCapability(
        WHvCapabilityCodeHypervisorPresent, &whpx_cap,
        sizeof(whpx_cap), &whpx_cap_size);
    if (FAILED(hr) || !whpx_cap.HypervisorPresent) {
        error_report("WHPX: No accelerator found, hr=%08lx", hr);
        ret = -ENOSPC;
        goto error;
    }

    hr = whp_dispatch.WHvCreatePartition(&whpx->partition);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to create partition, hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    /* Query the XSAVE capability of the partition. */
    hr = whp_dispatch.WHvGetPartitionProperty(
        whpx->partition,
        WHvPartitionPropertyCodeProcessorXsaveFeatures,
        &whpx_xsave_cap,
        sizeof(whpx_xsave_cap),
        &whpx_cap_size);

    /*
     * Windows version which don't support this property will return with the
     * specific error code.
     */
    if (FAILED(hr) && hr != WHV_E_UNKNOWN_PROPERTY) {
        error_report("WHPX: Failed to query XSAVE capability, hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    /* Disable xsave until the WHPX xsave API's have not been plumbed in */
    whpx_disable_xsave(whpx);

    memset(&prop, 0, sizeof(WHV_PARTITION_PROPERTY));
    prop.ProcessorCount = smp_cpus;
    hr = whp_dispatch.WHvSetPartitionProperty(
        whpx->partition,
        WHvPartitionPropertyCodeProcessorCount,
        &prop,
        sizeof(WHV_PARTITION_PROPERTY));

    if (FAILED(hr)) {
        error_report("WHPX: Failed to set partition core count to %d,"
                     " hr=%08lx", smp_cores, hr);
        ret = -EINVAL;
        goto error;
    }

    ret = whpx_init_kernel_irqchip(ms, whpx);
    if (ret) {
        goto error;
    }

    memset(&prop, 0, sizeof(WHV_PARTITION_PROPERTY));
    prop.ExtendedVmExits.X64MsrExit = 1;
    prop.ExtendedVmExits.X64CpuidExit = 1;
    hr = whp_dispatch.WHvSetPartitionProperty(
        whpx->partition,
        WHvPartitionPropertyCodeExtendedVmExits,
        &prop,
        sizeof(WHV_PARTITION_PROPERTY));

    if (FAILED(hr)) {
        error_report("WHPX: Failed to enable partition extended X64MsrExit and"
                     " X64CpuidExit hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    UINT32 cpuidExitList[] = {1, 7, WHPX_CPUID_SIGNATURE, 0x80000001};
    hr = whp_dispatch.WHvSetPartitionProperty(
        whpx->partition,
        WHvPartitionPropertyCodeCpuidExitList,
        cpuidExitList,
        RTL_NUMBER_OF(cpuidExitList) * sizeof(UINT32));

    if (FAILED(hr)) {
        error_report("WHPX: Failed to enable partition extended CpuidExitList"
                     " hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    if (FAILED(hr)) {
        error_report("WHPX: Failed to enable partition extended X64MsrExit and CpuidExit's"
                     " hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    hr = whp_dispatch.WHvSetupPartition(whpx->partition);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to setup partition, hr=%08lx", hr);
        ret = -EINVAL;
        goto error;
    }

    whpx_memory_init();

    qemu_add_vm_change_state_handler(whpx_vm_state_change, NULL);

    cpu_interrupt_handler = whpx_handle_interrupt;

    printf("Windows Hypervisor Platform accelerator is operational\n");
    return 0;

  error:

    whpx_kernel_irqchip = false;

    if (NULL != whpx->partition) {
        whp_dispatch.WHvDeletePartition(whpx->partition);
        whpx->partition = NULL;
    }


    return ret;
}

int whpx_enabled(void)
{
    return whpx_allowed;
}

static void whpx_accel_class_init(ObjectClass *oc, void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);
    ac->name = "WHPX";
    ac->init_machine = whpx_accel_init;
    ac->allowed = &whpx_allowed;
}

static const TypeInfo whpx_accel_type = {
    .name = ACCEL_CLASS_NAME("whpx"),
    .parent = TYPE_ACCEL,
    .class_init = whpx_accel_class_init,
};

static void whpx_type_init(void)
{
    type_register_static(&whpx_accel_type);
    type_register_static(&whpx_apic_info);
}

bool init_whp_dispatch() {
    char* lib_name;
    HMODULE hLib;

    if(whp_dispatch_initialized)
        return true;

    #define WHP_LOAD_FIELD(return_type, function_name, signature) \
        whp_dispatch.function_name = (function_name ## _t)GetProcAddress(hLib, #function_name); \
        if (!whp_dispatch.function_name) { \
            error_report("Could not load function %s from library %s.", \
                         #function_name, lib_name); \
            goto error; \
        } \

    lib_name = "WinHvPlatform.dll";
    hWinHvPlatform = LoadLibrary(lib_name);
    if (!hWinHvPlatform) {
        error_report("Could not load library %s.", lib_name);
        goto error;
    }
    hLib = hWinHvPlatform;
    LIST_WINHVPLATFORM_FUNCTIONS(WHP_LOAD_FIELD)

    #define WHP_LOAD_FIELD_OPTIONAL(return_type, function_name, signature) \
        whp_dispatch.function_name = \
            (function_name ## _t)GetProcAddress(hLib, #function_name); \

    LIST_WINHVPLATFORM_FUNCTIONS_SUPPLEMENTAL(WHP_LOAD_FIELD_OPTIONAL)

    lib_name = "WinHvEmulation.dll";
    HMODULE hWinHvEmulation = LoadLibrary(lib_name);
    if (!hWinHvEmulation) {
        error_report("Could not load library %s.", lib_name);
        goto error;
    }
    hLib = hWinHvEmulation;
    LIST_WINHVEMULATION_FUNCTIONS(WHP_LOAD_FIELD)

    whp_dispatch_initialized = true;
    return true;

    error:

    if (hWinHvPlatform)
        FreeLibrary(hWinHvPlatform);
    if (hWinHvEmulation)
        FreeLibrary(hWinHvEmulation);
    return false;
}

type_init(whpx_type_init);
