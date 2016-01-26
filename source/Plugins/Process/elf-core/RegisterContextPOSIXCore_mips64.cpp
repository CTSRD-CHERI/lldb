//===-- RegisterContextCorePOSIX_mips64.cpp ---------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/DataExtractor.h"
#include "lldb/Core/RegisterValue.h"
#include "lldb/Target/Thread.h"
#include "RegisterContextPOSIXCore_mips64.h"

using namespace lldb_private;

RegisterContextCorePOSIX_mips64::RegisterContextCorePOSIX_mips64(Thread &thread,
                                                                 RegisterInfoInterface *register_info,
                                                                 const DataExtractor &gpregset,
                                                                 const DataExtractor &fpregset,
                                                                 const DataExtractor &capregset)
    : RegisterContextPOSIX_mips64(thread, 0, register_info)
{
    m_gpr_buffer.reset(new DataBufferHeap(gpregset.GetDataStart(), gpregset.GetByteSize()));
    m_gpr.SetData(m_gpr_buffer);
    m_gpr.SetByteOrder(gpregset.GetByteOrder());

    m_cr_buffer.reset(new DataBufferHeap(capregset.GetDataStart(), capregset.GetByteSize()));
    m_cr.SetData(m_cr_buffer);
    m_cr.SetByteOrder(capregset.GetByteOrder());

    m_in_bd = lldb_private::eLazyBoolCalculate;
}

RegisterContextCorePOSIX_mips64::~RegisterContextCorePOSIX_mips64()
{
}

bool
RegisterContextCorePOSIX_mips64::ReadGPR()
{
    return true;
}

bool
RegisterContextCorePOSIX_mips64::ReadFPR()
{
    return false;
}

bool
RegisterContextCorePOSIX_mips64::WriteGPR()
{
    assert(0);
    return false;
}

bool
RegisterContextCorePOSIX_mips64::WriteFPR()
{
    assert(0);
    return false;
}

bool
RegisterContextCorePOSIX_mips64::CauseBD()
{
    uint32_t reg_num;
    reg_num = ConvertRegisterKindToRegisterNumber (lldb::eRegisterKindDWARF, dwarf_cause_mips64);
    const RegisterInfo *reg_info = GetRegisterInfoAtIndex (reg_num);
    RegisterValue reg_value;
    
    if (!ReadRegister(reg_info, reg_value))
        return false;

    if (reg_value.GetAsUInt64() & 0x80000000)
        return true;

    return false;
}

bool
RegisterContextCorePOSIX_mips64::ReadRegister(const RegisterInfo *reg_info, RegisterValue &value)
{
    const uint32_t reg = reg_info->kinds[lldb::eRegisterKindLLDB];
    lldb::offset_t offset = reg_info->byte_offset;
    if (reg == syn_pc_mips64)
    {
        // Synthetic PC is the sum of:
        // (a) GP reg PC
        assert(reg_info->kinds[lldb::eRegisterKindGeneric] == LLDB_REGNUM_GENERIC_PC);
        const RegisterInfo *pc_reg_info = GetRegisterInfoAtIndex(gpr_pc_mips64);
        RegisterValue reg_value;
        if (!ReadRegister(pc_reg_info, reg_value))
            return false;
        uint64_t v = reg_value.GetAsUInt64();

        // (b) Cap reg PCC base
        const RegisterInfo *pcc_reg_info = GetRegisterInfoAtIndex(cap_pcc_mips64);
        assert(pcc_reg_info->byte_size == 32);
        offset = pcc_reg_info->byte_offset + 16;
        uint64_t pcc_base = m_cr.GetMaxU64(&offset, 8);
        if (offset == pcc_reg_info->byte_offset + 24)
            v += pcc_base;

        // (c) Offset if Cause indicates exception occurred in branch delay slot
        if (m_in_bd == lldb_private::eLazyBoolCalculate)
            m_in_bd = CauseBD() ? lldb_private::eLazyBoolYes : lldb_private::eLazyBoolNo;
        if (m_in_bd == lldb_private::eLazyBoolYes)
            v += 4;

        value = v;
        return true;
    }
    else if (reg <= gpr_dummy_mips64)
    {
        uint64_t v = m_gpr.GetMaxU64(&offset, reg_info->byte_size);
        if (offset == reg_info->byte_offset + reg_info->byte_size)
        {
            value = v;
            return true;
        }
    }
    else
    {
        uint8_t buf[32];
        assert(reg_info->byte_size == 32);
        if (m_cr.CopyData(offset, 32, buf) == reg_info->byte_size)
        {
            value.SetBytes(buf, 32, m_cr.GetByteOrder());
            return true;
        }
    }
    return false;
}

bool
RegisterContextCorePOSIX_mips64::ReadAllRegisterValues(lldb::DataBufferSP &data_sp)
{
    return false;
}

bool
RegisterContextCorePOSIX_mips64::WriteRegister(const RegisterInfo *reg_info, const RegisterValue &value)
{
    return false;
}

bool
RegisterContextCorePOSIX_mips64::WriteAllRegisterValues(const lldb::DataBufferSP &data_sp)
{
    return false;
}

bool
RegisterContextCorePOSIX_mips64::HardwareSingleStep(bool enable)
{
    return false;
}
