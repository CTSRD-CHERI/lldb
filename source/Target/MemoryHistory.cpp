//===-- MemoryHistory.cpp -------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/MemoryHistory.h"

#include "lldb/Core/PluginManager.h"

using namespace lldb;
using namespace lldb_private;

lldb::MemoryHistorySP
MemoryHistory::FindPlugin (const ProcessSP process)
{
    MemoryHistoryCreateInstance create_callback = NULL;
    
    for (uint32_t idx = 0; (create_callback = PluginManager::GetMemoryHistoryCreateCallbackAtIndex(idx)) != NULL; ++idx)
    {
        return create_callback(process);
    }
    
    return MemoryHistorySP();
}
