//===-- GDBRemoteCommunicationServerLLGS.cpp --------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <errno.h>

#include "lldb/Host/Config.h"

#include "GDBRemoteCommunicationServerLLGS.h"
#include "lldb/Core/StreamGDBRemote.h"

// C Includes
// C++ Includes
#include <cstring>
#include <chrono>
#include <thread>

// Other libraries and framework includes
#include "llvm/ADT/Triple.h"
#include "lldb/Interpreter/Args.h"
#include "lldb/Core/DataBuffer.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/RegisterValue.h"
#include "lldb/Core/State.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Host/ConnectionFileDescriptor.h"
#include "lldb/Host/Debug.h"
#include "lldb/Host/Endian.h"
#include "lldb/Host/File.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Host/StringConvert.h"
#include "lldb/Host/TimeValue.h"
#include "lldb/Target/FileAction.h"
#include "lldb/Target/MemoryRegionInfo.h"
#include "lldb/Target/Platform.h"
#include "lldb/Host/common/NativeRegisterContext.h"
#include "lldb/Host/common/NativeProcessProtocol.h"
#include "lldb/Host/common/NativeThreadProtocol.h"

// Project includes
#include "Utility/StringExtractorGDBRemote.h"
#include "Utility/UriParser.h"
#include "ProcessGDBRemote.h"
#include "ProcessGDBRemoteLog.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::process_gdb_remote;
using namespace llvm;

//----------------------------------------------------------------------
// GDBRemote Errors
//----------------------------------------------------------------------

namespace
{
    enum GDBRemoteServerError
    {
        // Set to the first unused error number in literal form below
        eErrorFirst = 29,
        eErrorNoProcess = eErrorFirst,
        eErrorResume,
        eErrorExitStatus
    };
}

//----------------------------------------------------------------------
// GDBRemoteCommunicationServerLLGS constructor
//----------------------------------------------------------------------
GDBRemoteCommunicationServerLLGS::GDBRemoteCommunicationServerLLGS(
        const lldb::PlatformSP& platform_sp) :
    GDBRemoteCommunicationServerCommon ("gdb-remote.server", "gdb-remote.server.rx_packet"),
    m_platform_sp (platform_sp),
    m_async_thread (LLDB_INVALID_HOST_THREAD),
    m_current_tid (LLDB_INVALID_THREAD_ID),
    m_continue_tid (LLDB_INVALID_THREAD_ID),
    m_debugged_process_mutex (Mutex::eMutexTypeRecursive),
    m_debugged_process_sp (),
    m_stdio_communication ("process.stdio"),
    m_inferior_prev_state (StateType::eStateInvalid),
    m_active_auxv_buffer_sp (),
    m_saved_registers_mutex (),
    m_saved_registers_map (),
    m_next_saved_registers_id (1)
{
    assert(platform_sp);
    RegisterPacketHandlers();
}

//----------------------------------------------------------------------
// Destructor
//----------------------------------------------------------------------
GDBRemoteCommunicationServerLLGS::~GDBRemoteCommunicationServerLLGS()
{
    Mutex::Locker locker (m_debugged_process_mutex);

    if (m_debugged_process_sp)
    {
        m_debugged_process_sp->Terminate ();
        m_debugged_process_sp.reset ();
    }
}

void
GDBRemoteCommunicationServerLLGS::RegisterPacketHandlers()
{
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_C,
                                  &GDBRemoteCommunicationServerLLGS::Handle_C);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_c,
                                  &GDBRemoteCommunicationServerLLGS::Handle_c);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_D,
                                  &GDBRemoteCommunicationServerLLGS::Handle_D);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_H,
                                  &GDBRemoteCommunicationServerLLGS::Handle_H);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_I,
                                  &GDBRemoteCommunicationServerLLGS::Handle_I);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_interrupt,
                                  &GDBRemoteCommunicationServerLLGS::Handle_interrupt);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_m,
                                  &GDBRemoteCommunicationServerLLGS::Handle_m);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_M,
                                  &GDBRemoteCommunicationServerLLGS::Handle_M);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_p,
                                  &GDBRemoteCommunicationServerLLGS::Handle_p);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_P,
                                  &GDBRemoteCommunicationServerLLGS::Handle_P);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_qC,
                                  &GDBRemoteCommunicationServerLLGS::Handle_qC);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_qfThreadInfo,
                                  &GDBRemoteCommunicationServerLLGS::Handle_qfThreadInfo);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_qFileLoadAddress,
                                  &GDBRemoteCommunicationServerLLGS::Handle_qFileLoadAddress);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_qGetWorkingDir,
                                  &GDBRemoteCommunicationServerLLGS::Handle_qGetWorkingDir);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_qMemoryRegionInfo,
                                  &GDBRemoteCommunicationServerLLGS::Handle_qMemoryRegionInfo);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_qMemoryRegionInfoSupported,
                                  &GDBRemoteCommunicationServerLLGS::Handle_qMemoryRegionInfoSupported);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_qProcessInfo,
                                  &GDBRemoteCommunicationServerLLGS::Handle_qProcessInfo);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_qRegisterInfo,
                                  &GDBRemoteCommunicationServerLLGS::Handle_qRegisterInfo);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_QRestoreRegisterState,
                                  &GDBRemoteCommunicationServerLLGS::Handle_QRestoreRegisterState);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_QSaveRegisterState,
                                  &GDBRemoteCommunicationServerLLGS::Handle_QSaveRegisterState);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_QSetDisableASLR,
                                  &GDBRemoteCommunicationServerLLGS::Handle_QSetDisableASLR);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_QSetWorkingDir,
                                  &GDBRemoteCommunicationServerLLGS::Handle_QSetWorkingDir);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_qsThreadInfo,
                                  &GDBRemoteCommunicationServerLLGS::Handle_qsThreadInfo);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_qThreadStopInfo,
                                  &GDBRemoteCommunicationServerLLGS::Handle_qThreadStopInfo);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_qWatchpointSupportInfo,
                                  &GDBRemoteCommunicationServerLLGS::Handle_qWatchpointSupportInfo);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_qXfer_auxv_read,
                                  &GDBRemoteCommunicationServerLLGS::Handle_qXfer_auxv_read);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_s,
                                  &GDBRemoteCommunicationServerLLGS::Handle_s);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_stop_reason,
                                  &GDBRemoteCommunicationServerLLGS::Handle_stop_reason); // ?
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_vAttach,
                                  &GDBRemoteCommunicationServerLLGS::Handle_vAttach);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_vCont,
                                  &GDBRemoteCommunicationServerLLGS::Handle_vCont);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_vCont_actions,
                                  &GDBRemoteCommunicationServerLLGS::Handle_vCont_actions);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_Z,
                                  &GDBRemoteCommunicationServerLLGS::Handle_Z);
    RegisterMemberFunctionHandler(StringExtractorGDBRemote::eServerPacketType_z,
                                  &GDBRemoteCommunicationServerLLGS::Handle_z);

    RegisterPacketHandler(StringExtractorGDBRemote::eServerPacketType_k,
                          [this](StringExtractorGDBRemote packet,
                                 Error &error,
                                 bool &interrupt,
                                 bool &quit)
                          {
                              quit = true;
                              return this->Handle_k (packet);
                          });
}

Error
GDBRemoteCommunicationServerLLGS::SetLaunchArguments (const char *const args[], int argc)
{
    if ((argc < 1) || !args || !args[0] || !args[0][0])
        return Error ("%s: no process command line specified to launch", __FUNCTION__);

    m_process_launch_info.SetArguments (const_cast<const char**> (args), true);
    return Error ();
}

Error
GDBRemoteCommunicationServerLLGS::SetLaunchFlags (unsigned int launch_flags)
{
    m_process_launch_info.GetFlags ().Set (launch_flags);
    return Error ();
}

Error
GDBRemoteCommunicationServerLLGS::LaunchProcess ()
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));

    if (!m_process_launch_info.GetArguments ().GetArgumentCount ())
        return Error ("%s: no process command line specified to launch", __FUNCTION__);

    Error error;
    {
        Mutex::Locker locker (m_debugged_process_mutex);
        assert (!m_debugged_process_sp && "lldb-gdbserver creating debugged process but one already exists");
        error = m_platform_sp->LaunchNativeProcess (
            m_process_launch_info,
            *this,
            m_debugged_process_sp);
    }

    if (!error.Success ())
    {
        fprintf (stderr, "%s: failed to launch executable %s", __FUNCTION__, m_process_launch_info.GetArguments ().GetArgumentAtIndex (0));
        return error;
    }

    // Handle mirroring of inferior stdout/stderr over the gdb-remote protocol
    // as needed.
    // llgs local-process debugging may specify PTY paths, which will make these
    // file actions non-null
    // process launch -i/e/o will also make these file actions non-null
    // nullptr means that the traffic is expected to flow over gdb-remote protocol
    if (
        m_process_launch_info.GetFileActionForFD(STDIN_FILENO) == nullptr  ||
        m_process_launch_info.GetFileActionForFD(STDOUT_FILENO) == nullptr  ||
        m_process_launch_info.GetFileActionForFD(STDERR_FILENO) == nullptr
        )
    {
        // nullptr means it's not redirected to file or pty (in case of LLGS local)
        // at least one of stdio will be transferred pty<->gdb-remote
        // we need to give the pty master handle to this object to read and/or write
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " setting up stdout/stderr redirection via $O gdb-remote commands", __FUNCTION__, m_debugged_process_sp->GetID ());

        // Setup stdout/stderr mapping from inferior to $O
        auto terminal_fd = m_debugged_process_sp->GetTerminalFileDescriptor ();
        if (terminal_fd >= 0)
        {
            if (log)
                log->Printf ("ProcessGDBRemoteCommunicationServerLLGS::%s setting inferior STDIO fd to %d", __FUNCTION__, terminal_fd);
            error = SetSTDIOFileDescriptor (terminal_fd);
            if (error.Fail ())
                return error;
        }
        else
        {
            if (log)
                log->Printf ("ProcessGDBRemoteCommunicationServerLLGS::%s ignoring inferior STDIO since terminal fd reported as %d", __FUNCTION__, terminal_fd);
        }
    }
    else
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " skipping stdout/stderr redirection via $O: inferior will communicate over client-provided file descriptors", __FUNCTION__, m_debugged_process_sp->GetID ());
    }

    printf ("Launched '%s' as process %" PRIu64 "...\n", m_process_launch_info.GetArguments ().GetArgumentAtIndex (0), m_process_launch_info.GetProcessID ());

    // Add to list of spawned processes.
    lldb::pid_t pid;
    if ((pid = m_process_launch_info.GetProcessID ()) != LLDB_INVALID_PROCESS_ID)
    {
        // add to spawned pids
        Mutex::Locker locker (m_spawned_pids_mutex);
        // On an lldb-gdbserver, we would expect there to be only one.
        assert (m_spawned_pids.empty () && "lldb-gdbserver adding tracked process but one already existed");
        m_spawned_pids.insert (pid);
    }

    return error;
}

Error
GDBRemoteCommunicationServerLLGS::AttachToProcess (lldb::pid_t pid)
{
    Error error;

    Log *log (GetLogIfAnyCategoriesSet (LIBLLDB_LOG_PROCESS));
    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64, __FUNCTION__, pid);

    // Scope for mutex locker.
    {
        // Before we try to attach, make sure we aren't already monitoring something else.
        Mutex::Locker locker (m_spawned_pids_mutex);
        if (!m_spawned_pids.empty ())
        {
            error.SetErrorStringWithFormat ("cannot attach to a process %" PRIu64 " when another process with pid %" PRIu64 " is being debugged.", pid, *m_spawned_pids.begin());
            return error;
        }

        // Try to attach.
        error = m_platform_sp->AttachNativeProcess (pid, *this, m_debugged_process_sp);
        if (!error.Success ())
        {
            fprintf (stderr, "%s: failed to attach to process %" PRIu64 ": %s", __FUNCTION__, pid, error.AsCString ());
            return error;
        }

        // Setup stdout/stderr mapping from inferior.
        auto terminal_fd = m_debugged_process_sp->GetTerminalFileDescriptor ();
        if (terminal_fd >= 0)
        {
            if (log)
                log->Printf ("ProcessGDBRemoteCommunicationServerLLGS::%s setting inferior STDIO fd to %d", __FUNCTION__, terminal_fd);
            error = SetSTDIOFileDescriptor (terminal_fd);
            if (error.Fail ())
                return error;
        }
        else
        {
            if (log)
                log->Printf ("ProcessGDBRemoteCommunicationServerLLGS::%s ignoring inferior STDIO since terminal fd reported as %d", __FUNCTION__, terminal_fd);
        }

        printf ("Attached to process %" PRIu64 "...\n", pid);

        // Add to list of spawned processes.
        assert (m_spawned_pids.empty () && "lldb-gdbserver adding tracked process but one already existed");
        m_spawned_pids.insert (pid);

        return error;
    }
}

void
GDBRemoteCommunicationServerLLGS::InitializeDelegate (NativeProcessProtocol *process)
{
    assert (process && "process cannot be NULL");
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));
    if (log)
    {
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s called with NativeProcessProtocol pid %" PRIu64 ", current state: %s",
                __FUNCTION__,
                process->GetID (),
                StateAsCString (process->GetState ()));
    }
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::SendWResponse (NativeProcessProtocol *process)
{
    assert (process && "process cannot be NULL");
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));

    // send W notification
    ExitType exit_type = ExitType::eExitTypeInvalid;
    int return_code = 0;
    std::string exit_description;

    const bool got_exit_info = process->GetExitStatus (&exit_type, &return_code, exit_description);
    if (!got_exit_info)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 ", failed to retrieve process exit status", __FUNCTION__, process->GetID ());

        StreamGDBRemote response;
        response.PutChar ('E');
        response.PutHex8 (GDBRemoteServerError::eErrorExitStatus);
        return SendPacketNoLock(response.GetData(), response.GetSize());
    }
    else
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 ", returning exit type %d, return code %d [%s]", __FUNCTION__, process->GetID (), exit_type, return_code, exit_description.c_str ());

        StreamGDBRemote response;

        char return_type_code;
        switch (exit_type)
        {
            case ExitType::eExitTypeExit:
                return_type_code = 'W';
                break;
            case ExitType::eExitTypeSignal:
                return_type_code = 'X';
                break;
            case ExitType::eExitTypeStop:
                return_type_code = 'S';
                break;
            case ExitType::eExitTypeInvalid:
                return_type_code = 'E';
                break;
        }
        response.PutChar (return_type_code);

        // POSIX exit status limited to unsigned 8 bits.
        response.PutHex8 (return_code);

        return SendPacketNoLock(response.GetData(), response.GetSize());
    }
}

static void
AppendHexValue (StreamString &response, const uint8_t* buf, uint32_t buf_size, bool swap)
{
    int64_t i;
    if (swap)
    {
        for (i = buf_size-1; i >= 0; i--)
            response.PutHex8 (buf[i]);
    }
    else
    {
        for (i = 0; i < buf_size; i++)
            response.PutHex8 (buf[i]);
    }
}

static void
WriteRegisterValueInHexFixedWidth (StreamString &response,
                                   NativeRegisterContextSP &reg_ctx_sp,
                                   const RegisterInfo &reg_info,
                                   const RegisterValue *reg_value_p)
{
    RegisterValue reg_value;
    if (!reg_value_p)
    {
        Error error = reg_ctx_sp->ReadRegister (&reg_info, reg_value);
        if (error.Success ())
            reg_value_p = &reg_value;
        // else log.
    }

    if (reg_value_p)
    {
        AppendHexValue (response, (const uint8_t*) reg_value_p->GetBytes (), reg_value_p->GetByteSize (), false);
    }
    else
    {
        // Zero-out any unreadable values.
        if (reg_info.byte_size > 0)
        {
            std::basic_string<uint8_t> zeros(reg_info.byte_size, '\0');
            AppendHexValue (response, zeros.data(), zeros.size(), false);
        }
    }
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::SendStopReplyPacketForThread (lldb::tid_t tid)
{
    Log *log (GetLogIfAnyCategoriesSet (LIBLLDB_LOG_PROCESS | LIBLLDB_LOG_THREAD));

    // Ensure we have a debugged process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
        return SendErrorResponse (50);

    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s preparing packet for pid %" PRIu64 " tid %" PRIu64,
                __FUNCTION__, m_debugged_process_sp->GetID (), tid);

    // Ensure we can get info on the given thread.
    NativeThreadProtocolSP thread_sp (m_debugged_process_sp->GetThreadByID (tid));
    if (!thread_sp)
        return SendErrorResponse (51);

    // Grab the reason this thread stopped.
    struct ThreadStopInfo tid_stop_info;
    std::string description;
    if (!thread_sp->GetStopReason (tid_stop_info, description))
        return SendErrorResponse (52);

    // FIXME implement register handling for exec'd inferiors.
    // if (tid_stop_info.reason == eStopReasonExec)
    // {
    //     const bool force = true;
    //     InitializeRegisters(force);
    // }

    StreamString response;
    // Output the T packet with the thread
    response.PutChar ('T');
    int signum = tid_stop_info.details.signal.signo;
    if (log)
    {
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " tid %" PRIu64 " got signal signo = %d, reason = %d, exc_type = %" PRIu64, 
                __FUNCTION__,
                m_debugged_process_sp->GetID (),
                tid,
                signum,
                tid_stop_info.reason,
                tid_stop_info.details.exception.type);
    }

    // Print the signal number.
    response.PutHex8 (signum & 0xff);

    // Include the tid.
    response.Printf ("thread:%" PRIx64 ";", tid);

    // Include the thread name if there is one.
    const std::string thread_name = thread_sp->GetName ();
    if (!thread_name.empty ())
    {
        size_t thread_name_len = thread_name.length ();

        if (::strcspn (thread_name.c_str (), "$#+-;:") == thread_name_len)
        {
            response.PutCString ("name:");
            response.PutCString (thread_name.c_str ());
        }
        else
        {
            // The thread name contains special chars, send as hex bytes.
            response.PutCString ("hexname:");
            response.PutCStringAsRawHex8 (thread_name.c_str ());
        }
        response.PutChar (';');
    }

    // If a 'QListThreadsInStopReply' was sent to enable this feature, we
    // will send all thread IDs back in the "threads" key whose value is
    // a list of hex thread IDs separated by commas:
    //  "threads:10a,10b,10c;"
    // This will save the debugger from having to send a pair of qfThreadInfo
    // and qsThreadInfo packets, but it also might take a lot of room in the
    // stop reply packet, so it must be enabled only on systems where there
    // are no limits on packet lengths.
    if (m_list_threads_in_stop_reply)
    {
        response.PutCString ("threads:");

        uint32_t thread_index = 0;
        NativeThreadProtocolSP listed_thread_sp;
        for (listed_thread_sp = m_debugged_process_sp->GetThreadAtIndex (thread_index); listed_thread_sp; ++thread_index, listed_thread_sp = m_debugged_process_sp->GetThreadAtIndex (thread_index))
        {
            if (thread_index > 0)
                response.PutChar (',');
            response.Printf ("%" PRIx64, listed_thread_sp->GetID ());
        }
        response.PutChar (';');
    }

    //
    // Expedite registers.
    //

    // Grab the register context.
    NativeRegisterContextSP reg_ctx_sp = thread_sp->GetRegisterContext ();
    if (reg_ctx_sp)
    {
        // Expedite all registers in the first register set (i.e. should be GPRs) that are not contained in other registers.
        const RegisterSet *reg_set_p;
        if (reg_ctx_sp->GetRegisterSetCount () > 0 && ((reg_set_p = reg_ctx_sp->GetRegisterSet (0)) != nullptr))
        {
            if (log)
                log->Printf ("GDBRemoteCommunicationServerLLGS::%s expediting registers from set '%s' (registers set count: %zu)", __FUNCTION__, reg_set_p->name ? reg_set_p->name : "<unnamed-set>", reg_set_p->num_registers);

            for (const uint32_t *reg_num_p = reg_set_p->registers; *reg_num_p != LLDB_INVALID_REGNUM; ++reg_num_p)
            {
                const RegisterInfo *const reg_info_p = reg_ctx_sp->GetRegisterInfoAtIndex (*reg_num_p);
                if (reg_info_p == nullptr)
                {
                    if (log)
                        log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed to get register info for register set '%s', register index %" PRIu32, __FUNCTION__, reg_set_p->name ? reg_set_p->name : "<unnamed-set>", *reg_num_p);
                }
                else if (reg_info_p->value_regs == nullptr)
                {
                    // Only expediate registers that are not contained in other registers.
                    RegisterValue reg_value;
                    Error error = reg_ctx_sp->ReadRegister (reg_info_p, reg_value);
                    if (error.Success ())
                    {
                        response.Printf ("%.02x:", *reg_num_p);
                        WriteRegisterValueInHexFixedWidth(response, reg_ctx_sp, *reg_info_p, &reg_value);
                        response.PutChar (';');
                    }
                    else
                    {
                        if (log)
                            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed to read register '%s' index %" PRIu32 ": %s", __FUNCTION__, reg_info_p->name ? reg_info_p->name : "<unnamed-register>", *reg_num_p, error.AsCString ());

                    }
                }
            }
        }
    }

    const char* reason_str = nullptr;
    switch (tid_stop_info.reason)
    {
    case eStopReasonTrace:
        reason_str = "trace";
        break;
    case eStopReasonBreakpoint:
        reason_str = "breakpoint";
        break;
    case eStopReasonWatchpoint:
        reason_str = "watchpoint";
        break;
    case eStopReasonSignal:
        reason_str = "signal";
        break;
    case eStopReasonException:
        reason_str = "exception";
        break;
    case eStopReasonExec:
        reason_str = "exec";
        break;
    case eStopReasonInstrumentation:
    case eStopReasonInvalid:
    case eStopReasonPlanComplete:
    case eStopReasonThreadExiting:
    case eStopReasonNone:
        break;
    }
    if (reason_str != nullptr)
    {
        response.Printf ("reason:%s;", reason_str);
    }

    if (!description.empty())
    {
        // Description may contains special chars, send as hex bytes.
        response.PutCString ("description:");
        response.PutCStringAsRawHex8 (description.c_str ());
        response.PutChar (';');
    }
    else if ((tid_stop_info.reason == eStopReasonException) && tid_stop_info.details.exception.type)
    {
        response.PutCString ("metype:");
        response.PutHex64 (tid_stop_info.details.exception.type);
        response.PutCString (";mecount:");
        response.PutHex32 (tid_stop_info.details.exception.data_count);
        response.PutChar (';');

        for (uint32_t i = 0; i < tid_stop_info.details.exception.data_count; ++i)
        {
            response.PutCString ("medata:");
            response.PutHex64 (tid_stop_info.details.exception.data[i]);
            response.PutChar (';');
        }
    }

    return SendPacketNoLock (response.GetData(), response.GetSize());
}

void
GDBRemoteCommunicationServerLLGS::HandleInferiorState_Exited (NativeProcessProtocol *process)
{
    assert (process && "process cannot be NULL");

    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));
    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s called", __FUNCTION__);

    // Send the exit result, and don't flush output.
    // Note: flushing output here would join the inferior stdio reflection thread, which
    // would gunk up the waitpid monitor thread that is calling this.
    PacketResult result = SendStopReasonForState (StateType::eStateExited, false);
    if (result != PacketResult::Success)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed to send stop notification for PID %" PRIu64 ", state: eStateExited", __FUNCTION__, process->GetID ());
    }

    // Remove the process from the list of spawned pids.
    {
        Mutex::Locker locker (m_spawned_pids_mutex);
        if (m_spawned_pids.erase (process->GetID ()) < 1)
        {
            if (log)
                log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed to remove PID %" PRIu64 " from the spawned pids list", __FUNCTION__, process->GetID ());

        }
    }

    // FIXME can't do this yet - since process state propagation is currently
    // synchronous, it is running off the NativeProcessProtocol's innards and
    // will tear down the NPP while it still has code to execute.
#if 0
    // Clear the NativeProcessProtocol pointer.
    {
        Mutex::Locker locker (m_debugged_process_mutex);
        m_debugged_process_sp.reset();
    }
#endif

    // Close the pipe to the inferior terminal i/o if we launched it
    // and set one up.  Otherwise, 'k' and its flush of stdio could
    // end up waiting on a thread join that will never end.  Consider
    // adding a timeout to the connection thread join call so we
    // can avoid that scenario altogether.
    MaybeCloseInferiorTerminalConnection ();

    // We are ready to exit the debug monitor.
    m_exit_now = true;
}

void
GDBRemoteCommunicationServerLLGS::HandleInferiorState_Stopped (NativeProcessProtocol *process)
{
    assert (process && "process cannot be NULL");

    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));
    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s called", __FUNCTION__);

    // Send the stop reason unless this is the stop after the
    // launch or attach.
    switch (m_inferior_prev_state)
    {
        case eStateLaunching:
        case eStateAttaching:
            // Don't send anything per debugserver behavior.
            break;
        default:
            // In all other cases, send the stop reason.
            PacketResult result = SendStopReasonForState (StateType::eStateStopped, false);
            if (result != PacketResult::Success)
            {
                if (log)
                    log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed to send stop notification for PID %" PRIu64 ", state: eStateExited", __FUNCTION__, process->GetID ());
            }
            break;
    }
}

void
GDBRemoteCommunicationServerLLGS::ProcessStateChanged (NativeProcessProtocol *process, lldb::StateType state)
{
    assert (process && "process cannot be NULL");
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));
    if (log)
    {
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s called with NativeProcessProtocol pid %" PRIu64 ", state: %s",
                __FUNCTION__,
                process->GetID (),
                StateAsCString (state));
    }

    // Make sure we get all of the pending stdout/stderr from the inferior
    // and send it to the lldb host before we send the state change
    // notification
    m_stdio_communication.SynchronizeWithReadThread();

    switch (state)
    {
    case StateType::eStateExited:
        HandleInferiorState_Exited (process);
        break;

    case StateType::eStateStopped:
        HandleInferiorState_Stopped (process);
        break;

    default:
        if (log)
        {
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s didn't handle state change for pid %" PRIu64 ", new state: %s",
                    __FUNCTION__,
                    process->GetID (),
                    StateAsCString (state));
        }
        break;
    }

    // Remember the previous state reported to us.
    m_inferior_prev_state = state;
}

void
GDBRemoteCommunicationServerLLGS::DidExec (NativeProcessProtocol *process)
{
    ClearProcessSpecificData ();
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::SendONotification (const char *buffer, uint32_t len)
{
    if ((buffer == nullptr) || (len == 0))
    {
        // Nothing to send.
        return PacketResult::Success;
    }

    StreamString response;
    response.PutChar ('O');
    response.PutBytesAsRawHex8 (buffer, len);

    return SendPacketNoLock (response.GetData (), response.GetSize ());
}

Error
GDBRemoteCommunicationServerLLGS::SetSTDIOFileDescriptor (int fd)
{
    Error error;

    // Set up the Read Thread for reading/handling process I/O
    std::unique_ptr<ConnectionFileDescriptor> conn_up (new ConnectionFileDescriptor (fd, true));
    if (!conn_up)
    {
        error.SetErrorString ("failed to create ConnectionFileDescriptor");
        return error;
    }

    m_stdio_communication.SetCloseOnEOF (false);
    m_stdio_communication.SetConnection (conn_up.release());
    if (!m_stdio_communication.IsConnected ())
    {
        error.SetErrorString ("failed to set connection for inferior I/O communication");
        return error;
    }

    // llgs local-process debugging may specify PTY paths, which will make these
    // file actions non-null
    // process launch -e/o will also make these file actions non-null
    // nullptr means that the traffic is expected to flow over gdb-remote protocol
    if (
        m_process_launch_info.GetFileActionForFD(STDOUT_FILENO) == nullptr ||
        m_process_launch_info.GetFileActionForFD(STDERR_FILENO) == nullptr
        )
    {
        // output from the process must be forwarded over gdb-remote
        // create a thread to read the handle and send the data
        m_stdio_communication.SetReadThreadBytesReceivedCallback (STDIOReadThreadBytesReceived, this);
        m_stdio_communication.StartReadThread();
    }

    return error;
}

void
GDBRemoteCommunicationServerLLGS::STDIOReadThreadBytesReceived (void *baton, const void *src, size_t src_len)
{
    GDBRemoteCommunicationServerLLGS *server = reinterpret_cast<GDBRemoteCommunicationServerLLGS*> (baton);
    static_cast<void> (server->SendONotification (static_cast<const char *>(src), src_len));
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_qProcessInfo (StringExtractorGDBRemote &packet)
{
    // Fail if we don't have a current process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
        return SendErrorResponse (68);

    lldb::pid_t pid = m_debugged_process_sp->GetID ();

    if (pid == LLDB_INVALID_PROCESS_ID)
        return SendErrorResponse (1);

    ProcessInstanceInfo proc_info;
    if (!Host::GetProcessInfo (pid, proc_info))
        return SendErrorResponse (1);

    StreamString response;
    CreateProcessInfoResponse_DebugServerStyle(proc_info, response);
    return SendPacketNoLock (response.GetData (), response.GetSize ());
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_qC (StringExtractorGDBRemote &packet)
{
    // Fail if we don't have a current process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
        return SendErrorResponse (68);

    // Make sure we set the current thread so g and p packets return
    // the data the gdb will expect.
    lldb::tid_t tid = m_debugged_process_sp->GetCurrentThreadID ();
    SetCurrentThreadID (tid);

    NativeThreadProtocolSP thread_sp = m_debugged_process_sp->GetCurrentThread ();
    if (!thread_sp)
        return SendErrorResponse (69);

    StreamString response;
    response.Printf ("QC%" PRIx64, thread_sp->GetID ());

    return SendPacketNoLock (response.GetData(), response.GetSize());
}

bool
GDBRemoteCommunicationServerLLGS::DebuggedProcessReaped (lldb::pid_t pid)
{
    // reap a process that we were debugging (but not debugserver)
    Mutex::Locker locker (m_spawned_pids_mutex);
    return m_spawned_pids.erase(pid) > 0;
}

bool
GDBRemoteCommunicationServerLLGS::ReapDebuggedProcess (void *callback_baton,
                                        lldb::pid_t pid,
                                        bool exited,
                                        int signal,    // Zero for no signal
                                        int status)    // Exit value of process if signal is zero
{
    GDBRemoteCommunicationServerLLGS *server = (GDBRemoteCommunicationServerLLGS *)callback_baton;
    server->DebuggedProcessReaped (pid);
    return true;
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_k (StringExtractorGDBRemote &packet)
{
    // shutdown all spawned processes
    std::set<lldb::pid_t> spawned_pids_copy;

    // copy pids
    {
        Mutex::Locker locker (m_spawned_pids_mutex);
        spawned_pids_copy.insert (m_spawned_pids.begin (), m_spawned_pids.end ());
    }

    // nuke the spawned processes
    for (auto it = spawned_pids_copy.begin (); it != spawned_pids_copy.end (); ++it)
    {
        lldb::pid_t spawned_pid = *it;
        if (!KillSpawnedProcess (spawned_pid))
        {
            fprintf (stderr, "%s: failed to kill spawned pid %" PRIu64 ", ignoring.\n", __FUNCTION__, spawned_pid);
        }
    }

    FlushInferiorOutput ();

    // No OK response for kill packet.
    // return SendOKResponse ();
    return PacketResult::Success;
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_QSetDisableASLR (StringExtractorGDBRemote &packet)
{
    packet.SetFilePos(::strlen ("QSetDisableASLR:"));
    if (packet.GetU32(0))
        m_process_launch_info.GetFlags().Set (eLaunchFlagDisableASLR);
    else
        m_process_launch_info.GetFlags().Clear (eLaunchFlagDisableASLR);
    return SendOKResponse ();
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_QSetWorkingDir (StringExtractorGDBRemote &packet)
{
    packet.SetFilePos (::strlen ("QSetWorkingDir:"));
    std::string path;
    packet.GetHexByteString (path);
    m_process_launch_info.SetWorkingDirectory(FileSpec{path, true});
    return SendOKResponse ();
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_qGetWorkingDir (StringExtractorGDBRemote &packet)
{
    FileSpec working_dir{m_process_launch_info.GetWorkingDirectory()};
    if (working_dir)
    {
        StreamString response;
        response.PutCStringAsRawHex8(working_dir.GetCString());
        return SendPacketNoLock(response.GetData(), response.GetSize());
    }

    return SendErrorResponse(14);
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_C (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS|LIBLLDB_LOG_THREAD));
    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s called", __FUNCTION__);

    // Ensure we have a native process.
    if (!m_debugged_process_sp)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s no debugged process shared pointer", __FUNCTION__);
        return SendErrorResponse (0x36);
    }

    // Pull out the signal number.
    packet.SetFilePos (::strlen ("C"));
    if (packet.GetBytesLeft () < 1)
    {
        // Shouldn't be using a C without a signal.
        return SendIllFormedResponse (packet, "C packet specified without signal.");
    }
    const uint32_t signo = packet.GetHexMaxU32 (false, std::numeric_limits<uint32_t>::max ());
    if (signo == std::numeric_limits<uint32_t>::max ())
        return SendIllFormedResponse (packet, "failed to parse signal number");

    // Handle optional continue address.
    if (packet.GetBytesLeft () > 0)
    {
        // FIXME add continue at address support for $C{signo}[;{continue-address}].
        if (*packet.Peek () == ';')
            return SendUnimplementedResponse (packet.GetStringRef().c_str());
        else
            return SendIllFormedResponse (packet, "unexpected content after $C{signal-number}");
    }

    ResumeActionList resume_actions (StateType::eStateRunning, 0);
    Error error;

    // We have two branches: what to do if a continue thread is specified (in which case we target
    // sending the signal to that thread), or when we don't have a continue thread set (in which
    // case we send a signal to the process).

    // TODO discuss with Greg Clayton, make sure this makes sense.

    lldb::tid_t signal_tid = GetContinueThreadID ();
    if (signal_tid != LLDB_INVALID_THREAD_ID)
    {
        // The resume action for the continue thread (or all threads if a continue thread is not set).
        ResumeAction action = { GetContinueThreadID (), StateType::eStateRunning, static_cast<int> (signo) };

        // Add the action for the continue thread (or all threads when the continue thread isn't present).
        resume_actions.Append (action);
    }
    else
    {
        // Send the signal to the process since we weren't targeting a specific continue thread with the signal.
        error = m_debugged_process_sp->Signal (signo);
        if (error.Fail ())
        {
            if (log)
                log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed to send signal for process %" PRIu64 ": %s",
                             __FUNCTION__,
                             m_debugged_process_sp->GetID (),
                             error.AsCString ());

            return SendErrorResponse (0x52);
        }
    }

    // Resume the threads.
    error = m_debugged_process_sp->Resume (resume_actions);
    if (error.Fail ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed to resume threads for process %" PRIu64 ": %s",
                         __FUNCTION__,
                         m_debugged_process_sp->GetID (),
                         error.AsCString ());

        return SendErrorResponse (0x38);
    }

    // Don't send an "OK" packet; response is the stopped/exited message.
    return PacketResult::Success;
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_c (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS|LIBLLDB_LOG_THREAD));
    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s called", __FUNCTION__);

    packet.SetFilePos (packet.GetFilePos() + ::strlen ("c"));

    // For now just support all continue.
    const bool has_continue_address = (packet.GetBytesLeft () > 0);
    if (has_continue_address)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s not implemented for c{address} variant [%s remains]", __FUNCTION__, packet.Peek ());
        return SendUnimplementedResponse (packet.GetStringRef().c_str());
    }

    // Ensure we have a native process.
    if (!m_debugged_process_sp)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s no debugged process shared pointer", __FUNCTION__);
        return SendErrorResponse (0x36);
    }

    // Build the ResumeActionList
    ResumeActionList actions (StateType::eStateRunning, 0);

    Error error = m_debugged_process_sp->Resume (actions);
    if (error.Fail ())
    {
        if (log)
        {
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s c failed for process %" PRIu64 ": %s",
                         __FUNCTION__,
                         m_debugged_process_sp->GetID (),
                         error.AsCString ());
        }
        return SendErrorResponse (GDBRemoteServerError::eErrorResume);
    }

    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s continued process %" PRIu64, __FUNCTION__, m_debugged_process_sp->GetID ());

    // No response required from continue.
    return PacketResult::Success;
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_vCont_actions (StringExtractorGDBRemote &packet)
{
    StreamString response;
    response.Printf("vCont;c;C;s;S");

    return SendPacketNoLock(response.GetData(), response.GetSize());
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_vCont (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));
    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s handling vCont packet", __FUNCTION__);

    packet.SetFilePos (::strlen ("vCont"));

    if (packet.GetBytesLeft() == 0)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s missing action from vCont package", __FUNCTION__);
        return SendIllFormedResponse (packet, "Missing action from vCont package");
    }

    // Check if this is all continue (no options or ";c").
    if (::strcmp (packet.Peek (), ";c") == 0)
    {
        // Move past the ';', then do a simple 'c'.
        packet.SetFilePos (packet.GetFilePos () + 1);
        return Handle_c (packet);
    }
    else if (::strcmp (packet.Peek (), ";s") == 0)
    {
        // Move past the ';', then do a simple 's'.
        packet.SetFilePos (packet.GetFilePos () + 1);
        return Handle_s (packet);
    }

    // Ensure we have a native process.
    if (!m_debugged_process_sp)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s no debugged process shared pointer", __FUNCTION__);
        return SendErrorResponse (0x36);
    }

    ResumeActionList thread_actions;

    while (packet.GetBytesLeft () && *packet.Peek () == ';')
    {
        // Skip the semi-colon.
        packet.GetChar ();

        // Build up the thread action.
        ResumeAction thread_action;
        thread_action.tid = LLDB_INVALID_THREAD_ID;
        thread_action.state = eStateInvalid;
        thread_action.signal = 0;

        const char action = packet.GetChar ();
        switch (action)
        {
            case 'C':
                thread_action.signal = packet.GetHexMaxU32 (false, 0);
                if (thread_action.signal == 0)
                    return SendIllFormedResponse (packet, "Could not parse signal in vCont packet C action");
                // Fall through to next case...

            case 'c':
                // Continue
                thread_action.state = eStateRunning;
                break;

            case 'S':
                thread_action.signal = packet.GetHexMaxU32 (false, 0);
                if (thread_action.signal == 0)
                    return SendIllFormedResponse (packet, "Could not parse signal in vCont packet S action");
                // Fall through to next case...

            case 's':
                // Step
                thread_action.state = eStateStepping;
                break;

            default:
                return SendIllFormedResponse (packet, "Unsupported vCont action");
                break;
        }

        // Parse out optional :{thread-id} value.
        if (packet.GetBytesLeft () && (*packet.Peek () == ':'))
        {
            // Consume the separator.
            packet.GetChar ();

            thread_action.tid = packet.GetHexMaxU32 (false, LLDB_INVALID_THREAD_ID);
            if (thread_action.tid == LLDB_INVALID_THREAD_ID)
                return SendIllFormedResponse (packet, "Could not parse thread number in vCont packet");
        }

        thread_actions.Append (thread_action);
    }

    Error error = m_debugged_process_sp->Resume (thread_actions);
    if (error.Fail ())
    {
        if (log)
        {
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s vCont failed for process %" PRIu64 ": %s",
                         __FUNCTION__,
                         m_debugged_process_sp->GetID (),
                         error.AsCString ());
        }
        return SendErrorResponse (GDBRemoteServerError::eErrorResume);
    }

    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s continued process %" PRIu64, __FUNCTION__, m_debugged_process_sp->GetID ());

    // No response required from vCont.
    return PacketResult::Success;
}

void
GDBRemoteCommunicationServerLLGS::SetCurrentThreadID (lldb::tid_t tid)
{
    Log *log (GetLogIfAnyCategoriesSet (LIBLLDB_LOG_THREAD));
    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s setting current thread id to %" PRIu64, __FUNCTION__, tid);

    m_current_tid = tid;
    if (m_debugged_process_sp)
        m_debugged_process_sp->SetCurrentThreadID (m_current_tid);
}

void
GDBRemoteCommunicationServerLLGS::SetContinueThreadID (lldb::tid_t tid)
{
    Log *log (GetLogIfAnyCategoriesSet (LIBLLDB_LOG_THREAD));
    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s setting continue thread id to %" PRIu64, __FUNCTION__, tid);

    m_continue_tid = tid;
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_stop_reason (StringExtractorGDBRemote &packet)
{
    // Handle the $? gdbremote command.

    // If no process, indicate error
    if (!m_debugged_process_sp)
        return SendErrorResponse (02);

    return SendStopReasonForState (m_debugged_process_sp->GetState (), true);
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::SendStopReasonForState (lldb::StateType process_state, bool flush_on_exit)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));

    switch (process_state)
    {
        case eStateAttaching:
        case eStateLaunching:
        case eStateRunning:
        case eStateStepping:
        case eStateDetached:
            // NOTE: gdb protocol doc looks like it should return $OK
            // when everything is running (i.e. no stopped result).
            return PacketResult::Success;  // Ignore

        case eStateSuspended:
        case eStateStopped:
        case eStateCrashed:
        {
            lldb::tid_t tid = m_debugged_process_sp->GetCurrentThreadID ();
            // Make sure we set the current thread so g and p packets return
            // the data the gdb will expect.
            SetCurrentThreadID (tid);
            return SendStopReplyPacketForThread (tid);
        }

        case eStateInvalid:
        case eStateUnloaded:
        case eStateExited:
            if (flush_on_exit)
                FlushInferiorOutput ();
            return SendWResponse(m_debugged_process_sp.get());

        default:
            if (log)
            {
                log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 ", current state reporting not handled: %s",
                             __FUNCTION__,
                             m_debugged_process_sp->GetID (),
                             StateAsCString (process_state));
            }
            break;
    }
    
    return SendErrorResponse (0);
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_qRegisterInfo (StringExtractorGDBRemote &packet)
{
    // Fail if we don't have a current process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
        return SendErrorResponse (68);

    // Ensure we have a thread.
    NativeThreadProtocolSP thread_sp (m_debugged_process_sp->GetThreadAtIndex (0));
    if (!thread_sp)
        return SendErrorResponse (69);

    // Get the register context for the first thread.
    NativeRegisterContextSP reg_context_sp (thread_sp->GetRegisterContext ());
    if (!reg_context_sp)
        return SendErrorResponse (69);

    // Parse out the register number from the request.
    packet.SetFilePos (strlen("qRegisterInfo"));
    const uint32_t reg_index = packet.GetHexMaxU32 (false, std::numeric_limits<uint32_t>::max ());
    if (reg_index == std::numeric_limits<uint32_t>::max ())
        return SendErrorResponse (69);

    // Return the end of registers response if we've iterated one past the end of the register set.
    if (reg_index >= reg_context_sp->GetUserRegisterCount ())
        return SendErrorResponse (69);

    const RegisterInfo *reg_info = reg_context_sp->GetRegisterInfoAtIndex(reg_index);
    if (!reg_info)
        return SendErrorResponse (69);

    // Build the reginfos response.
    StreamGDBRemote response;

    response.PutCString ("name:");
    response.PutCString (reg_info->name);
    response.PutChar (';');

    if (reg_info->alt_name && reg_info->alt_name[0])
    {
        response.PutCString ("alt-name:");
        response.PutCString (reg_info->alt_name);
        response.PutChar (';');
    }

    response.Printf ("bitsize:%" PRIu32 ";offset:%" PRIu32 ";", reg_info->byte_size * 8, reg_info->byte_offset);

    switch (reg_info->encoding)
    {
        case eEncodingUint:    response.PutCString ("encoding:uint;"); break;
        case eEncodingSint:    response.PutCString ("encoding:sint;"); break;
        case eEncodingIEEE754: response.PutCString ("encoding:ieee754;"); break;
        case eEncodingVector:  response.PutCString ("encoding:vector;"); break;
        default: break;
    }

    switch (reg_info->format)
    {
        case eFormatBinary:          response.PutCString ("format:binary;"); break;
        case eFormatDecimal:         response.PutCString ("format:decimal;"); break;
        case eFormatHex:             response.PutCString ("format:hex;"); break;
        case eFormatFloat:           response.PutCString ("format:float;"); break;
        case eFormatVectorOfSInt8:   response.PutCString ("format:vector-sint8;"); break;
        case eFormatVectorOfUInt8:   response.PutCString ("format:vector-uint8;"); break;
        case eFormatVectorOfSInt16:  response.PutCString ("format:vector-sint16;"); break;
        case eFormatVectorOfUInt16:  response.PutCString ("format:vector-uint16;"); break;
        case eFormatVectorOfSInt32:  response.PutCString ("format:vector-sint32;"); break;
        case eFormatVectorOfUInt32:  response.PutCString ("format:vector-uint32;"); break;
        case eFormatVectorOfFloat32: response.PutCString ("format:vector-float32;"); break;
        case eFormatVectorOfUInt128: response.PutCString ("format:vector-uint128;"); break;
        default: break;
    };

    const char *const register_set_name = reg_context_sp->GetRegisterSetNameForRegisterAtIndex(reg_index);
    if (register_set_name)
    {
        response.PutCString ("set:");
        response.PutCString (register_set_name);
        response.PutChar (';');
    }

    if (reg_info->kinds[RegisterKind::eRegisterKindGCC] != LLDB_INVALID_REGNUM)
        response.Printf ("gcc:%" PRIu32 ";", reg_info->kinds[RegisterKind::eRegisterKindGCC]);

    if (reg_info->kinds[RegisterKind::eRegisterKindDWARF] != LLDB_INVALID_REGNUM)
        response.Printf ("dwarf:%" PRIu32 ";", reg_info->kinds[RegisterKind::eRegisterKindDWARF]);

    switch (reg_info->kinds[RegisterKind::eRegisterKindGeneric])
    {
        case LLDB_REGNUM_GENERIC_PC:     response.PutCString("generic:pc;"); break;
        case LLDB_REGNUM_GENERIC_SP:     response.PutCString("generic:sp;"); break;
        case LLDB_REGNUM_GENERIC_FP:     response.PutCString("generic:fp;"); break;
        case LLDB_REGNUM_GENERIC_RA:     response.PutCString("generic:ra;"); break;
        case LLDB_REGNUM_GENERIC_FLAGS:  response.PutCString("generic:flags;"); break;
        case LLDB_REGNUM_GENERIC_ARG1:   response.PutCString("generic:arg1;"); break;
        case LLDB_REGNUM_GENERIC_ARG2:   response.PutCString("generic:arg2;"); break;
        case LLDB_REGNUM_GENERIC_ARG3:   response.PutCString("generic:arg3;"); break;
        case LLDB_REGNUM_GENERIC_ARG4:   response.PutCString("generic:arg4;"); break;
        case LLDB_REGNUM_GENERIC_ARG5:   response.PutCString("generic:arg5;"); break;
        case LLDB_REGNUM_GENERIC_ARG6:   response.PutCString("generic:arg6;"); break;
        case LLDB_REGNUM_GENERIC_ARG7:   response.PutCString("generic:arg7;"); break;
        case LLDB_REGNUM_GENERIC_ARG8:   response.PutCString("generic:arg8;"); break;
        default: break;
    }

    if (reg_info->value_regs && reg_info->value_regs[0] != LLDB_INVALID_REGNUM)
    {
        response.PutCString ("container-regs:");
        int i = 0;
        for (const uint32_t *reg_num = reg_info->value_regs; *reg_num != LLDB_INVALID_REGNUM; ++reg_num, ++i)
        {
            if (i > 0)
                response.PutChar (',');
            response.Printf ("%" PRIx32, *reg_num);
        }
        response.PutChar (';');
    }

    if (reg_info->invalidate_regs && reg_info->invalidate_regs[0])
    {
        response.PutCString ("invalidate-regs:");
        int i = 0;
        for (const uint32_t *reg_num = reg_info->invalidate_regs; *reg_num != LLDB_INVALID_REGNUM; ++reg_num, ++i)
        {
            if (i > 0)
                response.PutChar (',');
            response.Printf ("%" PRIx32, *reg_num);
        }
        response.PutChar (';');
    }

    return SendPacketNoLock(response.GetData(), response.GetSize());
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_qfThreadInfo (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_THREAD));

    // Fail if we don't have a current process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s() no process (%s), returning OK", __FUNCTION__, m_debugged_process_sp ? "invalid process id" : "null m_debugged_process_sp");
        return SendOKResponse ();
    }

    StreamGDBRemote response;
    response.PutChar ('m');

    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s() starting thread iteration", __FUNCTION__);

    NativeThreadProtocolSP thread_sp;
    uint32_t thread_index;
    for (thread_index = 0, thread_sp = m_debugged_process_sp->GetThreadAtIndex (thread_index);
         thread_sp;
         ++thread_index, thread_sp = m_debugged_process_sp->GetThreadAtIndex (thread_index))
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s() iterated thread %" PRIu32 "(%s, tid=0x%" PRIx64 ")", __FUNCTION__, thread_index, thread_sp ? "is not null" : "null", thread_sp ? thread_sp->GetID () : LLDB_INVALID_THREAD_ID);
        if (thread_index > 0)
            response.PutChar(',');
        response.Printf ("%" PRIx64, thread_sp->GetID ());
    }

    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s() finished thread iteration", __FUNCTION__);

    return SendPacketNoLock(response.GetData(), response.GetSize());
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_qsThreadInfo (StringExtractorGDBRemote &packet)
{
    // FIXME for now we return the full thread list in the initial packet and always do nothing here.
    return SendPacketNoLock ("l", 1);
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_p (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_THREAD));

    // Parse out the register number from the request.
    packet.SetFilePos (strlen("p"));
    const uint32_t reg_index = packet.GetHexMaxU32 (false, std::numeric_limits<uint32_t>::max ());
    if (reg_index == std::numeric_limits<uint32_t>::max ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, could not parse register number from request \"%s\"", __FUNCTION__, packet.GetStringRef ().c_str ());
        return SendErrorResponse (0x15);
    }

    // Get the thread to use.
    NativeThreadProtocolSP thread_sp = GetThreadFromSuffix (packet);
    if (!thread_sp)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no thread available", __FUNCTION__);
        return SendErrorResponse (0x15);
    }

    // Get the thread's register context.
    NativeRegisterContextSP reg_context_sp (thread_sp->GetRegisterContext ());
    if (!reg_context_sp)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " tid %" PRIu64 " failed, no register context available for the thread", __FUNCTION__, m_debugged_process_sp->GetID (), thread_sp->GetID ());
        return SendErrorResponse (0x15);
    }

    // Return the end of registers response if we've iterated one past the end of the register set.
    if (reg_index >= reg_context_sp->GetUserRegisterCount ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, requested register %" PRIu32 " beyond register count %" PRIu32, __FUNCTION__, reg_index, reg_context_sp->GetUserRegisterCount ());
        return SendErrorResponse (0x15);
    }

    const RegisterInfo *reg_info = reg_context_sp->GetRegisterInfoAtIndex(reg_index);
    if (!reg_info)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, requested register %" PRIu32 " returned NULL", __FUNCTION__, reg_index);
        return SendErrorResponse (0x15);
    }

    // Build the reginfos response.
    StreamGDBRemote response;

    // Retrieve the value
    RegisterValue reg_value;
    Error error = reg_context_sp->ReadRegister (reg_info, reg_value);
    if (error.Fail ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, read of requested register %" PRIu32 " (%s) failed: %s", __FUNCTION__, reg_index, reg_info->name, error.AsCString ());
        return SendErrorResponse (0x15);
    }

    const uint8_t *const data = reinterpret_cast<const uint8_t*> (reg_value.GetBytes ());
    if (!data)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed to get data bytes from requested register %" PRIu32, __FUNCTION__, reg_index);
        return SendErrorResponse (0x15);
    }

    // FIXME flip as needed to get data in big/little endian format for this host.
    for (uint32_t i = 0; i < reg_value.GetByteSize (); ++i)
        response.PutHex8 (data[i]);

    return SendPacketNoLock (response.GetData (), response.GetSize ());
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_P (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_THREAD));

    // Ensure there is more content.
    if (packet.GetBytesLeft () < 1)
        return SendIllFormedResponse (packet, "Empty P packet");

    // Parse out the register number from the request.
    packet.SetFilePos (strlen("P"));
    const uint32_t reg_index = packet.GetHexMaxU32 (false, std::numeric_limits<uint32_t>::max ());
    if (reg_index == std::numeric_limits<uint32_t>::max ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, could not parse register number from request \"%s\"", __FUNCTION__, packet.GetStringRef ().c_str ());
        return SendErrorResponse (0x29);
    }

    // Note debugserver would send an E30 here.
    if ((packet.GetBytesLeft () < 1) || (packet.GetChar () != '='))
        return SendIllFormedResponse (packet, "P packet missing '=' char after register number");

    // Get process architecture.
    ArchSpec process_arch;
    if (!m_debugged_process_sp || !m_debugged_process_sp->GetArchitecture (process_arch))
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed to retrieve inferior architecture", __FUNCTION__);
        return SendErrorResponse (0x49);
    }

    // Parse out the value.
    uint8_t reg_bytes[32]; // big enough to support up to 256 bit ymmN register
    size_t reg_size = packet.GetHexBytesAvail (reg_bytes, sizeof(reg_bytes));

    // Get the thread to use.
    NativeThreadProtocolSP thread_sp = GetThreadFromSuffix (packet);
    if (!thread_sp)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no thread available (thread index 0)", __FUNCTION__);
        return SendErrorResponse (0x28);
    }

    // Get the thread's register context.
    NativeRegisterContextSP reg_context_sp (thread_sp->GetRegisterContext ());
    if (!reg_context_sp)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " tid %" PRIu64 " failed, no register context available for the thread", __FUNCTION__, m_debugged_process_sp->GetID (), thread_sp->GetID ());
        return SendErrorResponse (0x15);
    }

    const RegisterInfo *reg_info = reg_context_sp->GetRegisterInfoAtIndex (reg_index);
    if (!reg_info)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, requested register %" PRIu32 " returned NULL", __FUNCTION__, reg_index);
        return SendErrorResponse (0x48);
    }

    // Return the end of registers response if we've iterated one past the end of the register set.
    if (reg_index >= reg_context_sp->GetUserRegisterCount ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, requested register %" PRIu32 " beyond register count %" PRIu32, __FUNCTION__, reg_index, reg_context_sp->GetUserRegisterCount ());
        return SendErrorResponse (0x47);
    }

    if (reg_size != reg_info->byte_size)
    {
        return SendIllFormedResponse (packet, "P packet register size is incorrect");
    }

    // Build the reginfos response.
    StreamGDBRemote response;

    RegisterValue reg_value (reg_bytes, reg_size, process_arch.GetByteOrder ());
    Error error = reg_context_sp->WriteRegister (reg_info, reg_value);
    if (error.Fail ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, write of requested register %" PRIu32 " (%s) failed: %s", __FUNCTION__, reg_index, reg_info->name, error.AsCString ());
        return SendErrorResponse (0x32);
    }

    return SendOKResponse();
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_H (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_THREAD));

    // Fail if we don't have a current process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no process available", __FUNCTION__);
        return SendErrorResponse (0x15);
    }

    // Parse out which variant of $H is requested.
    packet.SetFilePos (strlen("H"));
    if (packet.GetBytesLeft () < 1)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, H command missing {g,c} variant", __FUNCTION__);
        return SendIllFormedResponse (packet, "H command missing {g,c} variant");
    }

    const char h_variant = packet.GetChar ();
    switch (h_variant)
    {
        case 'g':
            break;

        case 'c':
            break;

        default:
            if (log)
                log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, invalid $H variant %c", __FUNCTION__, h_variant);
            return SendIllFormedResponse (packet, "H variant unsupported, should be c or g");
    }

    // Parse out the thread number.
    // FIXME return a parse success/fail value.  All values are valid here.
    const lldb::tid_t tid = packet.GetHexMaxU64 (false, std::numeric_limits<lldb::tid_t>::max ());

    // Ensure we have the given thread when not specifying -1 (all threads) or 0 (any thread).
    if (tid != LLDB_INVALID_THREAD_ID && tid != 0)
    {
        NativeThreadProtocolSP thread_sp (m_debugged_process_sp->GetThreadByID (tid));
        if (!thread_sp)
        {
            if (log)
                log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, tid %" PRIu64 " not found", __FUNCTION__, tid);
            return SendErrorResponse (0x15);
        }
    }

    // Now switch the given thread type.
    switch (h_variant)
    {
        case 'g':
            SetCurrentThreadID (tid);
            break;

        case 'c':
            SetContinueThreadID (tid);
            break;

        default:
            assert (false && "unsupported $H variant - shouldn't get here");
            return SendIllFormedResponse (packet, "H variant unsupported, should be c or g");
    }

    return SendOKResponse();
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_I (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_THREAD));

    // Fail if we don't have a current process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no process available", __FUNCTION__);
        return SendErrorResponse (0x15);
    }

    packet.SetFilePos (::strlen("I"));
    char tmp[4096];
    for (;;)
    {
        size_t read = packet.GetHexBytesAvail(tmp, sizeof(tmp));
        if (read == 0)
        {
            break;
        }
        // write directly to stdin *this might block if stdin buffer is full*
        // TODO: enqueue this block in circular buffer and send window size to remote host
        ConnectionStatus status;
        Error error;
        m_stdio_communication.Write(tmp, read, status, &error);
        if (error.Fail())
        {
            return SendErrorResponse (0x15);
        }
    }

    return SendOKResponse();
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_interrupt (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS | LIBLLDB_LOG_THREAD));

    // Fail if we don't have a current process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no process available", __FUNCTION__);
        return SendErrorResponse (0x15);
    }

    // Interrupt the process.
    Error error = m_debugged_process_sp->Interrupt ();
    if (error.Fail ())
    {
        if (log)
        {
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed for process %" PRIu64 ": %s",
                         __FUNCTION__,
                         m_debugged_process_sp->GetID (),
                         error.AsCString ());
        }
        return SendErrorResponse (GDBRemoteServerError::eErrorResume);
    }

    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s stopped process %" PRIu64, __FUNCTION__, m_debugged_process_sp->GetID ());

    // No response required from stop all.
    return PacketResult::Success;
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_m (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));

    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no process available", __FUNCTION__);
        return SendErrorResponse (0x15);
    }

    // Parse out the memory address.
    packet.SetFilePos (strlen("m"));
    if (packet.GetBytesLeft() < 1)
        return SendIllFormedResponse(packet, "Too short m packet");

    // Read the address.  Punting on validation.
    // FIXME replace with Hex U64 read with no default value that fails on failed read.
    const lldb::addr_t read_addr = packet.GetHexMaxU64(false, 0);

    // Validate comma.
    if ((packet.GetBytesLeft() < 1) || (packet.GetChar() != ','))
        return SendIllFormedResponse(packet, "Comma sep missing in m packet");

    // Get # bytes to read.
    if (packet.GetBytesLeft() < 1)
        return SendIllFormedResponse(packet, "Length missing in m packet");

    const uint64_t byte_count = packet.GetHexMaxU64(false, 0);
    if (byte_count == 0)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s nothing to read: zero-length packet", __FUNCTION__);
        return PacketResult::Success;
    }

    // Allocate the response buffer.
    std::string buf(byte_count, '\0');
    if (buf.empty())
        return SendErrorResponse (0x78);


    // Retrieve the process memory.
    size_t bytes_read = 0;
    Error error = m_debugged_process_sp->ReadMemoryWithoutTrap(read_addr, &buf[0], byte_count, bytes_read);
    if (error.Fail ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " mem 0x%" PRIx64 ": failed to read. Error: %s", __FUNCTION__, m_debugged_process_sp->GetID (), read_addr, error.AsCString ());
        return SendErrorResponse (0x08);
    }

    if (bytes_read == 0)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " mem 0x%" PRIx64 ": read 0 of %" PRIu64 " requested bytes", __FUNCTION__, m_debugged_process_sp->GetID (), read_addr, byte_count);
        return SendErrorResponse (0x08);
    }

    StreamGDBRemote response;
    for (size_t i = 0; i < bytes_read; ++i)
        response.PutHex8(buf[i]);

    return SendPacketNoLock(response.GetData(), response.GetSize());
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_M (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));

    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no process available", __FUNCTION__);
        return SendErrorResponse (0x15);
    }

    // Parse out the memory address.
    packet.SetFilePos (strlen("M"));
    if (packet.GetBytesLeft() < 1)
        return SendIllFormedResponse(packet, "Too short M packet");

    // Read the address.  Punting on validation.
    // FIXME replace with Hex U64 read with no default value that fails on failed read.
    const lldb::addr_t write_addr = packet.GetHexMaxU64(false, 0);

    // Validate comma.
    if ((packet.GetBytesLeft() < 1) || (packet.GetChar() != ','))
        return SendIllFormedResponse(packet, "Comma sep missing in M packet");

    // Get # bytes to read.
    if (packet.GetBytesLeft() < 1)
        return SendIllFormedResponse(packet, "Length missing in M packet");

    const uint64_t byte_count = packet.GetHexMaxU64(false, 0);
    if (byte_count == 0)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s nothing to write: zero-length packet", __FUNCTION__);
        return PacketResult::Success;
    }

    // Validate colon.
    if ((packet.GetBytesLeft() < 1) || (packet.GetChar() != ':'))
        return SendIllFormedResponse(packet, "Comma sep missing in M packet after byte length");

    // Allocate the conversion buffer.
    std::vector<uint8_t> buf(byte_count, 0);
    if (buf.empty())
        return SendErrorResponse (0x78);

    // Convert the hex memory write contents to bytes.
    StreamGDBRemote response;
    const uint64_t convert_count = packet.GetHexBytes(&buf[0], byte_count, 0);
    if (convert_count != byte_count)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " mem 0x%" PRIx64 ": asked to write %" PRIu64 " bytes, but only found %" PRIu64 " to convert.", __FUNCTION__, m_debugged_process_sp->GetID (), write_addr, byte_count, convert_count);
        return SendIllFormedResponse (packet, "M content byte length specified did not match hex-encoded content length");
    }

    // Write the process memory.
    size_t bytes_written = 0;
    Error error = m_debugged_process_sp->WriteMemory (write_addr, &buf[0], byte_count, bytes_written);
    if (error.Fail ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " mem 0x%" PRIx64 ": failed to write. Error: %s", __FUNCTION__, m_debugged_process_sp->GetID (), write_addr, error.AsCString ());
        return SendErrorResponse (0x09);
    }

    if (bytes_written == 0)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " mem 0x%" PRIx64 ": wrote 0 of %" PRIu64 " requested bytes", __FUNCTION__, m_debugged_process_sp->GetID (), write_addr, byte_count);
        return SendErrorResponse (0x09);
    }

    return SendOKResponse ();
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_qMemoryRegionInfoSupported (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));

    // Currently only the NativeProcessProtocol knows if it can handle a qMemoryRegionInfoSupported
    // request, but we're not guaranteed to be attached to a process.  For now we'll assume the
    // client only asks this when a process is being debugged.

    // Ensure we have a process running; otherwise, we can't figure this out
    // since we won't have a NativeProcessProtocol.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no process available", __FUNCTION__);
        return SendErrorResponse (0x15);
    }

    // Test if we can get any region back when asking for the region around NULL.
    MemoryRegionInfo region_info;
    const Error error = m_debugged_process_sp->GetMemoryRegionInfo (0, region_info);
    if (error.Fail ())
    {
        // We don't support memory region info collection for this NativeProcessProtocol.
        return SendUnimplementedResponse ("");
    }

    return SendOKResponse();
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_qMemoryRegionInfo (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));

    // Ensure we have a process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no process available", __FUNCTION__);
        return SendErrorResponse (0x15);
    }

    // Parse out the memory address.
    packet.SetFilePos (strlen("qMemoryRegionInfo:"));
    if (packet.GetBytesLeft() < 1)
        return SendIllFormedResponse(packet, "Too short qMemoryRegionInfo: packet");

    // Read the address.  Punting on validation.
    const lldb::addr_t read_addr = packet.GetHexMaxU64(false, 0);

    StreamGDBRemote response;

    // Get the memory region info for the target address.
    MemoryRegionInfo region_info;
    const Error error = m_debugged_process_sp->GetMemoryRegionInfo (read_addr, region_info);
    if (error.Fail ())
    {
        // Return the error message.

        response.PutCString ("error:");
        response.PutCStringAsRawHex8 (error.AsCString ());
        response.PutChar (';');
    }
    else
    {
        // Range start and size.
        response.Printf ("start:%" PRIx64 ";size:%" PRIx64 ";", region_info.GetRange ().GetRangeBase (), region_info.GetRange ().GetByteSize ());

        // Permissions.
        if (region_info.GetReadable () ||
            region_info.GetWritable () ||
            region_info.GetExecutable ())
        {
            // Write permissions info.
            response.PutCString ("permissions:");

            if (region_info.GetReadable ())
                response.PutChar ('r');
            if (region_info.GetWritable ())
                response.PutChar('w');
            if (region_info.GetExecutable())
                response.PutChar ('x');

            response.PutChar (';');
        }
    }

    return SendPacketNoLock(response.GetData(), response.GetSize());
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_Z (StringExtractorGDBRemote &packet)
{
    // Ensure we have a process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
    {
        Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no process available", __FUNCTION__);
        return SendErrorResponse (0x15);
    }

    // Parse out software or hardware breakpoint or watchpoint requested.
    packet.SetFilePos (strlen("Z"));
    if (packet.GetBytesLeft() < 1)
        return SendIllFormedResponse(packet, "Too short Z packet, missing software/hardware specifier");

    bool want_breakpoint = true;
    bool want_hardware = false;

    const GDBStoppointType stoppoint_type =
        GDBStoppointType(packet.GetS32 (eStoppointInvalid));
    switch (stoppoint_type)
    {
        case eBreakpointSoftware:
            want_hardware = false; want_breakpoint = true;  break;
        case eBreakpointHardware:
            want_hardware = true;  want_breakpoint = true;  break;
        case eWatchpointWrite:
            want_hardware = true;  want_breakpoint = false; break;
        case eWatchpointRead:
            want_hardware = true;  want_breakpoint = false; break;
        case eWatchpointReadWrite:
            want_hardware = true;  want_breakpoint = false; break;
        case eStoppointInvalid:
            return SendIllFormedResponse(packet, "Z packet had invalid software/hardware specifier");

    }

    if ((packet.GetBytesLeft() < 1) || packet.GetChar () != ',')
        return SendIllFormedResponse(packet, "Malformed Z packet, expecting comma after stoppoint type");

    // Parse out the stoppoint address.
    if (packet.GetBytesLeft() < 1)
        return SendIllFormedResponse(packet, "Too short Z packet, missing address");
    const lldb::addr_t addr = packet.GetHexMaxU64(false, 0);

    if ((packet.GetBytesLeft() < 1) || packet.GetChar () != ',')
        return SendIllFormedResponse(packet, "Malformed Z packet, expecting comma after address");

    // Parse out the stoppoint size (i.e. size hint for opcode size).
    const uint32_t size = packet.GetHexMaxU32 (false, std::numeric_limits<uint32_t>::max ());
    if (size == std::numeric_limits<uint32_t>::max ())
        return SendIllFormedResponse(packet, "Malformed Z packet, failed to parse size argument");

    if (want_breakpoint)
    {
        // Try to set the breakpoint.
        const Error error = m_debugged_process_sp->SetBreakpoint (addr, size, want_hardware);
        if (error.Success ())
            return SendOKResponse ();
        Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_BREAKPOINTS));
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64
                    " failed to set breakpoint: %s",
                    __FUNCTION__,
                    m_debugged_process_sp->GetID (),
                    error.AsCString ());
        return SendErrorResponse (0x09);
    }
    else
    {
        uint32_t watch_flags =
            stoppoint_type == eWatchpointWrite
            ? 0x1  // Write
            : 0x3; // ReadWrite

        // Try to set the watchpoint.
        const Error error = m_debugged_process_sp->SetWatchpoint (
                addr, size, watch_flags, want_hardware);
        if (error.Success ())
            return SendOKResponse ();
        Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_WATCHPOINTS));
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64
                    " failed to set watchpoint: %s",
                    __FUNCTION__,
                    m_debugged_process_sp->GetID (),
                    error.AsCString ());
        return SendErrorResponse (0x09);
    }
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_z (StringExtractorGDBRemote &packet)
{
    // Ensure we have a process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
    {
        Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no process available", __FUNCTION__);
        return SendErrorResponse (0x15);
    }

    // Parse out software or hardware breakpoint or watchpoint requested.
    packet.SetFilePos (strlen("z"));
    if (packet.GetBytesLeft() < 1)
        return SendIllFormedResponse(packet, "Too short z packet, missing software/hardware specifier");

    bool want_breakpoint = true;

    const GDBStoppointType stoppoint_type =
        GDBStoppointType(packet.GetS32 (eStoppointInvalid));
    switch (stoppoint_type)
    {
        case eBreakpointHardware:  want_breakpoint = true;  break;
        case eBreakpointSoftware:  want_breakpoint = true;  break;
        case eWatchpointWrite:     want_breakpoint = false; break;
        case eWatchpointRead:      want_breakpoint = false; break;
        case eWatchpointReadWrite: want_breakpoint = false; break;
        default:
            return SendIllFormedResponse(packet, "z packet had invalid software/hardware specifier");

    }

    if ((packet.GetBytesLeft() < 1) || packet.GetChar () != ',')
        return SendIllFormedResponse(packet, "Malformed z packet, expecting comma after stoppoint type");

    // Parse out the stoppoint address.
    if (packet.GetBytesLeft() < 1)
        return SendIllFormedResponse(packet, "Too short z packet, missing address");
    const lldb::addr_t addr = packet.GetHexMaxU64(false, 0);

    if ((packet.GetBytesLeft() < 1) || packet.GetChar () != ',')
        return SendIllFormedResponse(packet, "Malformed z packet, expecting comma after address");

    /*
    // Parse out the stoppoint size (i.e. size hint for opcode size).
    const uint32_t size = packet.GetHexMaxU32 (false, std::numeric_limits<uint32_t>::max ());
    if (size == std::numeric_limits<uint32_t>::max ())
        return SendIllFormedResponse(packet, "Malformed z packet, failed to parse size argument");
    */

    if (want_breakpoint)
    {
        // Try to clear the breakpoint.
        const Error error = m_debugged_process_sp->RemoveBreakpoint (addr);
        if (error.Success ())
            return SendOKResponse ();
        Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_BREAKPOINTS));
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64
                    " failed to remove breakpoint: %s",
                    __FUNCTION__,
                    m_debugged_process_sp->GetID (),
                    error.AsCString ());
        return SendErrorResponse (0x09);
    }
    else
    {
        // Try to clear the watchpoint.
        const Error error = m_debugged_process_sp->RemoveWatchpoint (addr);
        if (error.Success ())
            return SendOKResponse ();
        Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_WATCHPOINTS));
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64
                    " failed to remove watchpoint: %s",
                    __FUNCTION__,
                    m_debugged_process_sp->GetID (),
                    error.AsCString ());
        return SendErrorResponse (0x09);
    }
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_s (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS|LIBLLDB_LOG_THREAD));

    // Ensure we have a process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no process available", __FUNCTION__);
        return SendErrorResponse (0x32);
    }

    // We first try to use a continue thread id.  If any one or any all set, use the current thread.
    // Bail out if we don't have a thread id.
    lldb::tid_t tid = GetContinueThreadID ();
    if (tid == 0 || tid == LLDB_INVALID_THREAD_ID)
        tid = GetCurrentThreadID ();
    if (tid == LLDB_INVALID_THREAD_ID)
        return SendErrorResponse (0x33);

    // Double check that we have such a thread.
    // TODO investigate: on MacOSX we might need to do an UpdateThreads () here.
    NativeThreadProtocolSP thread_sp = m_debugged_process_sp->GetThreadByID (tid);
    if (!thread_sp || thread_sp->GetID () != tid)
        return SendErrorResponse (0x33);

    // Create the step action for the given thread.
    ResumeAction action = { tid, eStateStepping, 0 };

    // Setup the actions list.
    ResumeActionList actions;
    actions.Append (action);

    // All other threads stop while we're single stepping a thread.
    actions.SetDefaultThreadActionIfNeeded(eStateStopped, 0);
    Error error = m_debugged_process_sp->Resume (actions);
    if (error.Fail ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " tid %" PRIu64 " Resume() failed with error: %s", __FUNCTION__, m_debugged_process_sp->GetID (), tid, error.AsCString ());
        return SendErrorResponse(0x49);
    }

    // No response here - the stop or exit will come from the resulting action.
    return PacketResult::Success;
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_qXfer_auxv_read (StringExtractorGDBRemote &packet)
{
    // *BSD impls should be able to do this too.
#if defined(__linux__)
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));

    // Parse out the offset.
    packet.SetFilePos (strlen("qXfer:auxv:read::"));
    if (packet.GetBytesLeft () < 1)
        return SendIllFormedResponse (packet, "qXfer:auxv:read:: packet missing offset");

    const uint64_t auxv_offset = packet.GetHexMaxU64 (false, std::numeric_limits<uint64_t>::max ());
    if (auxv_offset == std::numeric_limits<uint64_t>::max ())
        return SendIllFormedResponse (packet, "qXfer:auxv:read:: packet missing offset");

    // Parse out comma.
    if (packet.GetBytesLeft () < 1 || packet.GetChar () != ',')
        return SendIllFormedResponse (packet, "qXfer:auxv:read:: packet missing comma after offset");

    // Parse out the length.
    const uint64_t auxv_length = packet.GetHexMaxU64 (false, std::numeric_limits<uint64_t>::max ());
    if (auxv_length == std::numeric_limits<uint64_t>::max ())
        return SendIllFormedResponse (packet, "qXfer:auxv:read:: packet missing length");

    // Grab the auxv data if we need it.
    if (!m_active_auxv_buffer_sp)
    {
        // Make sure we have a valid process.
        if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
        {
            if (log)
                log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no process available", __FUNCTION__);
            return SendErrorResponse (0x10);
        }

        // Grab the auxv data.
        m_active_auxv_buffer_sp = Host::GetAuxvData (m_debugged_process_sp->GetID ());
        if (!m_active_auxv_buffer_sp || m_active_auxv_buffer_sp->GetByteSize () ==  0)
        {
            // Hmm, no auxv data, call that an error.
            if (log)
                log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no auxv data retrieved", __FUNCTION__);
            m_active_auxv_buffer_sp.reset ();
            return SendErrorResponse (0x11);
        }
    }

    // FIXME find out if/how I lock the stream here.

    StreamGDBRemote response;
    bool done_with_buffer = false;

    if (auxv_offset >= m_active_auxv_buffer_sp->GetByteSize ())
    {
        // We have nothing left to send.  Mark the buffer as complete.
        response.PutChar ('l');
        done_with_buffer = true;
    }
    else
    {
        // Figure out how many bytes are available starting at the given offset.
        const uint64_t bytes_remaining = m_active_auxv_buffer_sp->GetByteSize () - auxv_offset;

        // Figure out how many bytes we're going to read.
        const uint64_t bytes_to_read = (auxv_length > bytes_remaining) ? bytes_remaining : auxv_length;

        // Mark the response type according to whether we're reading the remainder of the auxv data.
        if (bytes_to_read >= bytes_remaining)
        {
            // There will be nothing left to read after this
            response.PutChar ('l');
            done_with_buffer = true;
        }
        else
        {
            // There will still be bytes to read after this request.
            response.PutChar ('m');
        }

        // Now write the data in encoded binary form.
        response.PutEscapedBytes (m_active_auxv_buffer_sp->GetBytes () + auxv_offset, bytes_to_read);
    }

    if (done_with_buffer)
        m_active_auxv_buffer_sp.reset ();

    return SendPacketNoLock(response.GetData(), response.GetSize());
#else
    return SendUnimplementedResponse ("not implemented on this platform");
#endif
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_QSaveRegisterState (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_THREAD));

    // Move past packet name.
    packet.SetFilePos (strlen ("QSaveRegisterState"));

    // Get the thread to use.
    NativeThreadProtocolSP thread_sp = GetThreadFromSuffix (packet);
    if (!thread_sp)
    {
        if (m_thread_suffix_supported)
            return SendIllFormedResponse (packet, "No thread specified in QSaveRegisterState packet");
        else
            return SendIllFormedResponse (packet, "No thread was is set with the Hg packet");
    }

    // Grab the register context for the thread.
    NativeRegisterContextSP reg_context_sp (thread_sp->GetRegisterContext ());
    if (!reg_context_sp)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " tid %" PRIu64 " failed, no register context available for the thread", __FUNCTION__, m_debugged_process_sp->GetID (), thread_sp->GetID ());
        return SendErrorResponse (0x15);
    }

    // Save registers to a buffer.
    DataBufferSP register_data_sp;
    Error error = reg_context_sp->ReadAllRegisterValues (register_data_sp);
    if (error.Fail ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " failed to save all register values: %s", __FUNCTION__, m_debugged_process_sp->GetID (), error.AsCString ());
        return SendErrorResponse (0x75);
    }

    // Allocate a new save id.
    const uint32_t save_id = GetNextSavedRegistersID ();
    assert ((m_saved_registers_map.find (save_id) == m_saved_registers_map.end ()) && "GetNextRegisterSaveID() returned an existing register save id");

    // Save the register data buffer under the save id.
    {
        Mutex::Locker locker (m_saved_registers_mutex);
        m_saved_registers_map[save_id] = register_data_sp;
    }

    // Write the response.
    StreamGDBRemote response;
    response.Printf ("%" PRIu32, save_id);
    return SendPacketNoLock(response.GetData(), response.GetSize());
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_QRestoreRegisterState (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_THREAD));

    // Parse out save id.
    packet.SetFilePos (strlen ("QRestoreRegisterState:"));
    if (packet.GetBytesLeft () < 1)
        return SendIllFormedResponse (packet, "QRestoreRegisterState packet missing register save id");

    const uint32_t save_id = packet.GetU32 (0);
    if (save_id == 0)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s QRestoreRegisterState packet has malformed save id, expecting decimal uint32_t", __FUNCTION__);
        return SendErrorResponse (0x76);
    }

    // Get the thread to use.
    NativeThreadProtocolSP thread_sp = GetThreadFromSuffix (packet);
    if (!thread_sp)
    {
        if (m_thread_suffix_supported)
            return SendIllFormedResponse (packet, "No thread specified in QRestoreRegisterState packet");
        else
            return SendIllFormedResponse (packet, "No thread was is set with the Hg packet");
    }

    // Grab the register context for the thread.
    NativeRegisterContextSP reg_context_sp (thread_sp->GetRegisterContext ());
    if (!reg_context_sp)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " tid %" PRIu64 " failed, no register context available for the thread", __FUNCTION__, m_debugged_process_sp->GetID (), thread_sp->GetID ());
        return SendErrorResponse (0x15);
    }

    // Retrieve register state buffer, then remove from the list.
    DataBufferSP register_data_sp;
    {
        Mutex::Locker locker (m_saved_registers_mutex);

        // Find the register set buffer for the given save id.
        auto it = m_saved_registers_map.find (save_id);
        if (it == m_saved_registers_map.end ())
        {
            if (log)
                log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " does not have a register set save buffer for id %" PRIu32, __FUNCTION__, m_debugged_process_sp->GetID (), save_id);
            return SendErrorResponse (0x77);
        }
        register_data_sp = it->second;

        // Remove it from the map.
        m_saved_registers_map.erase (it);
    }

    Error error = reg_context_sp->WriteAllRegisterValues (register_data_sp);
    if (error.Fail ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s pid %" PRIu64 " failed to restore all register values: %s", __FUNCTION__, m_debugged_process_sp->GetID (), error.AsCString ());
        return SendErrorResponse (0x77);
    }

    return SendOKResponse();
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_vAttach (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));

    // Consume the ';' after vAttach.
    packet.SetFilePos (strlen ("vAttach"));
    if (!packet.GetBytesLeft () || packet.GetChar () != ';')
        return SendIllFormedResponse (packet, "vAttach missing expected ';'");

    // Grab the PID to which we will attach (assume hex encoding).
    lldb::pid_t pid = packet.GetU32 (LLDB_INVALID_PROCESS_ID, 16);
    if (pid == LLDB_INVALID_PROCESS_ID)
        return SendIllFormedResponse (packet, "vAttach failed to parse the process id");

    // Attempt to attach.
    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s attempting to attach to pid %" PRIu64, __FUNCTION__, pid);

    Error error = AttachToProcess (pid);

    if (error.Fail ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed to attach to pid %" PRIu64 ": %s\n", __FUNCTION__, pid, error.AsCString());
        return SendErrorResponse (0x01);
    }

    // Notify we attached by sending a stop packet.
    return SendStopReasonForState (m_debugged_process_sp->GetState (), true);
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_D (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet (LIBLLDB_LOG_PROCESS));

    // Scope for mutex locker.
    Mutex::Locker locker (m_spawned_pids_mutex);

    // Fail if we don't have a current process.
    if (!m_debugged_process_sp || (m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID))
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, no process available", __FUNCTION__);
        return SendErrorResponse (0x15);
    }

    if (m_spawned_pids.find(m_debugged_process_sp->GetID ()) == m_spawned_pids.end())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed to find PID %" PRIu64 " in spawned pids list",
                         __FUNCTION__, m_debugged_process_sp->GetID ());
        return SendErrorResponse (0x1);
    }

    lldb::pid_t pid = LLDB_INVALID_PROCESS_ID;

    // Consume the ';' after D.
    packet.SetFilePos (1);
    if (packet.GetBytesLeft ())
    {
        if (packet.GetChar () != ';')
            return SendIllFormedResponse (packet, "D missing expected ';'");

        // Grab the PID from which we will detach (assume hex encoding).
        pid = packet.GetU32 (LLDB_INVALID_PROCESS_ID, 16);
        if (pid == LLDB_INVALID_PROCESS_ID)
            return SendIllFormedResponse (packet, "D failed to parse the process id");
    }

    if (pid != LLDB_INVALID_PROCESS_ID &&
        m_debugged_process_sp->GetID () != pid)
    {
        return SendIllFormedResponse (packet, "Invalid pid");
    }

    if (m_stdio_communication.IsConnected ())
    {
        m_stdio_communication.StopReadThread ();
    }

    const Error error = m_debugged_process_sp->Detach ();
    if (error.Fail ())
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed to detach from pid %" PRIu64 ": %s\n",
                         __FUNCTION__, m_debugged_process_sp->GetID (), error.AsCString ());
        return SendErrorResponse (0x01);
    }

    m_spawned_pids.erase (m_debugged_process_sp->GetID ());
    return SendOKResponse ();
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_qThreadStopInfo (StringExtractorGDBRemote &packet)
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_THREAD));

    packet.SetFilePos (strlen("qThreadStopInfo"));
    const lldb::tid_t tid = packet.GetHexMaxU32 (false, LLDB_INVALID_THREAD_ID);
    if (tid == LLDB_INVALID_THREAD_ID)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s failed, could not parse thread id from request \"%s\"", __FUNCTION__, packet.GetStringRef ().c_str ());
        return SendErrorResponse (0x15);
    }
    return SendStopReplyPacketForThread (tid);
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_qWatchpointSupportInfo (StringExtractorGDBRemote &packet)
{
    // Fail if we don't have a current process.
    if (!m_debugged_process_sp ||
            m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID)
        return SendErrorResponse (68);

    packet.SetFilePos(strlen("qWatchpointSupportInfo"));
    if (packet.GetBytesLeft() == 0)
        return SendOKResponse();
    if (packet.GetChar() != ':')
        return SendErrorResponse(67);

    uint32_t num = m_debugged_process_sp->GetMaxWatchpoints();
    StreamGDBRemote response;
    response.Printf ("num:%d;", num);
    return SendPacketNoLock(response.GetData(), response.GetSize());
}

GDBRemoteCommunication::PacketResult
GDBRemoteCommunicationServerLLGS::Handle_qFileLoadAddress (StringExtractorGDBRemote &packet)
{
    // Fail if we don't have a current process.
    if (!m_debugged_process_sp ||
            m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID)
        return SendErrorResponse(67);

    packet.SetFilePos(strlen("qFileLoadAddress:"));
    if (packet.GetBytesLeft() == 0)
        return SendErrorResponse(68);

    std::string file_name;
    packet.GetHexByteString(file_name);

    lldb::addr_t file_load_address = LLDB_INVALID_ADDRESS;
    Error error = m_debugged_process_sp->GetFileLoadAddress(file_name, file_load_address);
    if (error.Fail())
        return SendErrorResponse(69);

    if (file_load_address == LLDB_INVALID_ADDRESS)
        return SendErrorResponse(1); // File not loaded

    StreamGDBRemote response;
    response.PutHex64(file_load_address);
    return SendPacketNoLock(response.GetData(), response.GetSize());
}

void
GDBRemoteCommunicationServerLLGS::FlushInferiorOutput ()
{
    // If we're not monitoring an inferior's terminal, ignore this.
    if (!m_stdio_communication.IsConnected())
        return;

    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS | LIBLLDB_LOG_THREAD));
    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s() called", __FUNCTION__);

    // FIXME implement a timeout on the join.
    m_stdio_communication.JoinReadThread();
}

void
GDBRemoteCommunicationServerLLGS::MaybeCloseInferiorTerminalConnection ()
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS));

    // Tell the stdio connection to shut down.
    if (m_stdio_communication.IsConnected())
    {
        auto connection = m_stdio_communication.GetConnection();
        if (connection)
        {
            Error error;
            connection->Disconnect (&error);

            if (error.Success ())
            {
                if (log)
                    log->Printf ("GDBRemoteCommunicationServerLLGS::%s disconnect process terminal stdio - SUCCESS", __FUNCTION__);
            }
            else
            {
                if (log)
                    log->Printf ("GDBRemoteCommunicationServerLLGS::%s disconnect process terminal stdio - FAIL: %s", __FUNCTION__, error.AsCString ());
            }
        }
    }
}


NativeThreadProtocolSP
GDBRemoteCommunicationServerLLGS::GetThreadFromSuffix (StringExtractorGDBRemote &packet)
{
    NativeThreadProtocolSP thread_sp;

    // We have no thread if we don't have a process.
    if (!m_debugged_process_sp || m_debugged_process_sp->GetID () == LLDB_INVALID_PROCESS_ID)
        return thread_sp;

    // If the client hasn't asked for thread suffix support, there will not be a thread suffix.
    // Use the current thread in that case.
    if (!m_thread_suffix_supported)
    {
        const lldb::tid_t current_tid = GetCurrentThreadID ();
        if (current_tid == LLDB_INVALID_THREAD_ID)
            return thread_sp;
        else if (current_tid == 0)
        {
            // Pick a thread.
            return m_debugged_process_sp->GetThreadAtIndex (0);
        }
        else
            return m_debugged_process_sp->GetThreadByID (current_tid);
    }

    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_THREAD));

    // Parse out the ';'.
    if (packet.GetBytesLeft () < 1 || packet.GetChar () != ';')
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s gdb-remote parse error: expected ';' prior to start of thread suffix: packet contents = '%s'", __FUNCTION__, packet.GetStringRef ().c_str ());
        return thread_sp;
    }

    if (!packet.GetBytesLeft ())
        return thread_sp;

    // Parse out thread: portion.
    if (strncmp (packet.Peek (), "thread:", strlen("thread:")) != 0)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationServerLLGS::%s gdb-remote parse error: expected 'thread:' but not found, packet contents = '%s'", __FUNCTION__, packet.GetStringRef ().c_str ());
        return thread_sp;
    }
    packet.SetFilePos (packet.GetFilePos () + strlen("thread:"));
    const lldb::tid_t tid = packet.GetHexMaxU64(false, 0);
    if (tid != 0)
        return m_debugged_process_sp->GetThreadByID (tid);

    return thread_sp;
}

lldb::tid_t
GDBRemoteCommunicationServerLLGS::GetCurrentThreadID () const
{
    if (m_current_tid == 0 || m_current_tid == LLDB_INVALID_THREAD_ID)
    {
        // Use whatever the debug process says is the current thread id
        // since the protocol either didn't specify or specified we want
        // any/all threads marked as the current thread.
        if (!m_debugged_process_sp)
            return LLDB_INVALID_THREAD_ID;
        return m_debugged_process_sp->GetCurrentThreadID ();
    }
    // Use the specific current thread id set by the gdb remote protocol.
    return m_current_tid;
}

uint32_t
GDBRemoteCommunicationServerLLGS::GetNextSavedRegistersID ()
{
    Mutex::Locker locker (m_saved_registers_mutex);
    return m_next_saved_registers_id++;
}

void
GDBRemoteCommunicationServerLLGS::ClearProcessSpecificData ()
{
    Log *log (GetLogIfAnyCategoriesSet(LIBLLDB_LOG_PROCESS|GDBR_LOG_PROCESS));
    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s()", __FUNCTION__);

    // Clear any auxv cached data.
    // *BSD impls should be able to do this too.
#if defined(__linux__)
    if (log)
        log->Printf ("GDBRemoteCommunicationServerLLGS::%s clearing auxv buffer (previously %s)",
                     __FUNCTION__,
                     m_active_auxv_buffer_sp ? "was set" : "was not set");
    m_active_auxv_buffer_sp.reset ();
#endif
}

FileSpec
GDBRemoteCommunicationServerLLGS::FindModuleFile(const std::string& module_path,
                                                 const ArchSpec& arch)
{
    if (m_debugged_process_sp)
    {
        FileSpec file_spec;
        if (m_debugged_process_sp->GetLoadedModuleFileSpec(module_path.c_str(), file_spec).Success())
        {
            if (file_spec.Exists())
                return file_spec;
        }
    }

    return GDBRemoteCommunicationServerCommon::FindModuleFile(module_path, arch);
}
