#!/usr/bin/env python3
"""psp_api_arity.py: writes the "import_arity" table of recomp/infected_profile.json.

The number of integer argument registers (a0..a3, t0..t3) each PSP syscall takes, from the SDK prototypes.  The generator infers
an import's arity from the registers its call sites define; a thin wrapper that forwards its own incoming arguments
(sceAudioSetChannelDataLen(handle, len) called from a function that only loads the handle) defines fewer than the call takes,
and the argument that was not marshalled reached the HLE as whatever g_cpu.gpr held.  The profile arity is merged with the
site-derived one (the larger wins).  Over-stating an arity costs a move; under-stating it is a silent wrong argument.

usage: psp_api_arity.py [imports.tsv] [profile.json]   (rewrites the profile in place)
"""
import json, sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
A = {
 # InterruptManager
 'sceKernelRegisterSubIntrHandler': 4, 'sceKernelReleaseSubIntrHandler': 2, 'sceKernelEnableSubIntr': 2,
 # sceGe_user
 'sceGeEdramGetSize': 0, 'sceGeEdramGetAddr': 0, 'sceGeListEnQueue': 4, 'sceGeListEnQueueHead': 4, 'sceGeListUpdateStallAddr': 2,
 'sceGeListSync': 2, 'sceGeDrawSync': 1, 'sceGeBreak': 2, 'sceGeContinue': 0, 'sceGeSetCallback': 1, 'sceGeUnsetCallback': 1,
 # sceDisplay / sceCtrl / power / utils
 'sceDisplaySetMode': 3, 'sceDisplayGetFramePerSec': 0, 'sceDisplaySetFrameBuf': 4, 'sceDisplayGetVcount': 0, 'sceDisplayWaitVblankStart': 0,
 'sceCtrlSetSamplingCycle': 1, 'sceCtrlSetSamplingMode': 1, 'sceCtrlPeekBufferPositive': 2,
 'sceKernelPowerLock': 1, 'sceKernelPowerUnlock': 1, 'sceKernelPowerTick': 1, 'sceKernelVolatileMemTryLock': 3, 'sceKernelVolatileMemUnlock': 1,
 'sceKernelDcacheInvalidateRange': 2, 'sceKernelLibcClock': 0, 'sceKernelLibcTime': 1, 'sceKernelLibcGettimeofday': 2,
 'sceKernelDcacheWritebackAll': 0, 'sceKernelDcacheWritebackRange': 2,
 # ThreadManForUser
 'sceKernelCheckCallback': 0, 'sceKernelCreateThread': 6, 'sceKernelDeleteThread': 1, 'sceKernelStartThread': 3, 'sceKernelExitThread': 1,
 'sceKernelExitDeleteThread': 1, 'sceKernelTerminateDeleteThread': 1, 'sceKernelChangeThreadPriority': 2, 'sceKernelGetThreadId': 0,
 'sceKernelGetThreadExitStatus': 1, 'sceKernelSleepThread': 0, 'sceKernelReferThreadProfiler': 0, 'sceKernelReferGlobalProfiler': 0,
 'sceKernelWakeupThread': 1, 'sceKernelWaitThreadEnd': 2, 'sceKernelDelayThread': 1, 'sceKernelDelayThreadCB': 1,
 'sceKernelCreateSema': 5, 'sceKernelDeleteSema': 1, 'sceKernelSignalSema': 2, 'sceKernelWaitSema': 3, 'sceKernelWaitSemaCB': 3,
 'sceKernelPollSema': 2, 'sceKernelCreateEventFlag': 4, 'sceKernelDeleteEventFlag': 1, 'sceKernelSetEventFlag': 2,
 'sceKernelClearEventFlag': 2, 'sceKernelWaitEventFlag': 5, 'sceKernelReferEventFlagStatus': 2, 'sceKernelCreateCallback': 3,
 'sceKernelSysClock2USec': 3, 'sceKernelGetSystemTime': 1, 'sceKernelGetSystemTimeWide': 0,
 # SysMem, stdio, modules, loadexec, interrupts
 'sceKernelAllocPartitionMemory': 5, 'sceKernelFreePartitionMemory': 1, 'sceKernelGetBlockHeadAddr': 1,
 'sceKernelStdin': 0, 'sceKernelStdout': 0, 'sceKernelStderr': 0,
 'sceKernelLoadModule': 3, 'sceKernelStartModule': 5, 'sceKernelStopModule': 5, 'sceKernelUnloadModule': 1, 'sceKernelSelfStopUnloadModule': 3,
 'sceKernelLoadExec': 2, 'sceKernelExitGame': 0, 'sceKernelRegisterExitCallback': 1, 'sceKernelCpuSuspendIntr': 0, 'sceKernelCpuResumeIntr': 1,
 # IoFileMgrForUser (sceIoLseek takes its 64-bit offset in an even register pair: fd, pad, offset lo, offset hi, whence)
 'sceIoPollAsync': 2, 'sceIoOpenAsync': 3, 'sceIoRead': 3, 'sceIoReadAsync': 3, 'sceIoWrite': 3, 'sceIoLseek': 5, 'sceIoWaitAsync': 2,
 'sceIoGetstat': 2, 'sceIoWaitAsyncCB': 2, 'sceIoRename': 2, 'sceIoDevctl': 6, 'sceIoChangeAsyncPriority': 2, 'sceIoClose': 1,
 'sceIoCloseAsync': 1, 'sceIoOpen': 3,
 # networking
 'sceNetResolverInit': 0, 'sceNetResolverTerm': 0, 'sceNetResolverCreate': 3, 'sceNetResolverDelete': 1, 'sceNetResolverStartNtoA': 5,
 'sceNetApctlInit': 2, 'sceNetApctlTerm': 0, 'sceNetApctlGetInfo': 2, 'sceNetApctlAddHandler': 2, 'sceNetApctlDelHandler': 1,
 'sceNetApctlDisconnect': 0, 'sceNetApctlGetState': 1,
 'sceNetInetInit': 0, 'sceNetInetTerm': 0, 'sceNetInetAccept': 3, 'sceNetInetBind': 3, 'sceNetInetClose': 1, 'sceNetInetConnect': 3,
 'sceNetInetGetsockname': 3, 'sceNetInetGetsockopt': 5, 'sceNetInetListen': 2, 'sceNetInetPoll': 3, 'sceNetInetRecv': 4,
 'sceNetInetRecvfrom': 6, 'sceNetInetSelect': 5, 'sceNetInetSend': 4, 'sceNetInetSendto': 6, 'sceNetInetSetsockopt': 5,
 'sceNetInetShutdown': 2, 'sceNetInetSocket': 3, 'sceNetInetGetErrno': 0, 'sceNetInetInetAddr': 1, 'sceNetInetInetNtop': 4,
 'sceNetAdhocctlInit': 3, 'sceNetAdhocctlTerm': 0, 'sceNetAdhocctlConnect': 1, 'sceNetAdhocctlDisconnect': 0, 'sceNetAdhocctlAddHandler': 2,
 'sceNetAdhocctlDelHandler': 1, 'sceNetAdhocctlGetState': 1,
 'sceNetAdhocInit': 0, 'sceNetAdhocTerm': 0, 'sceNetAdhocPdpCreate': 4, 'sceNetAdhocPdpSend': 7, 'sceNetAdhocPdpRecv': 7,
 'sceNetAdhocPdpDelete': 2, 'sceNetAdhocGetPdpStat': 2,
 'sceNetInit': 5, 'sceNetTerm': 0, 'sceNetEtherNtostr': 2, 'sceNetGetLocalEtherAddr': 1, 'sceWlanGetSwitchState': 0, 'sceWlanGetEtherAddr': 1,
 # sceAtrac3plus
 'sceAtracReleaseAtracID': 1, 'sceAtracSetDataAndGetID': 2, 'sceAtracDecodeData': 5, 'sceAtracGetRemainFrame': 2,
 'sceAtracGetStreamDataInfo': 4, 'sceAtracAddStreamData': 2, 'sceAtracGetInternalErrorInfo': 2,
 # sceSasCore
 '__sceSasCore': 2, '__sceSasGetEndFlag': 1, '__sceSasSetVolume': 6, '__sceSasSetPitch': 3, '__sceSasSetVoice': 5, '__sceSasSetNoise': 3,
 '__sceSasSetADSR': 7, '__sceSasSetADSRmode': 7, '__sceSasSetSL': 3, '__sceSasGetEnvelopeHeight': 2, '__sceSasSetSimpleADSR': 4,
 '__sceSasInit': 5, '__sceSasSetKeyOff': 2, '__sceSasSetKeyOn': 2, '__sceSasRevVON': 3, '__sceSasRevEVOL': 3, '__sceSasRevType': 2,
 '__sceSasRevParam': 3,
 # sceAudio
 'sceAudioOutputBlocking': 3, 'sceAudioOutputPanned': 4, 'sceAudioOutputPannedBlocking': 4, 'sceAudioChReserve': 3, 'sceAudioChRelease': 1,
 'sceAudioGetChannelRestLength': 1, 'sceAudioSetChannelDataLen': 2, 'sceAudioChangeChannelConfig': 2, 'sceAudioChangeChannelVolume': 3,
 # sceUmdUser
 'sceUmdCheckMedium': 0, 'sceUmdActivate': 2, 'sceUmdDeactivate': 2, 'sceUmdWaitDriveStat': 1, 'sceUmdWaitDriveStatCB': 2,
 'sceUmdCancelWaitDriveStat': 0, 'sceUmdGetDriveStat': 0,
 # sceMpeg
 'sceMpegQueryStreamOffset': 3, 'sceMpegQueryStreamSize': 2, 'sceMpegInit': 0, 'sceMpegFinish': 0, 'sceMpegQueryMemSize': 1,
 'sceMpegCreate': 7, 'sceMpegDelete': 1, 'sceMpegRegistStream': 3, 'sceMpegUnRegistStream': 2, 'sceMpegMallocAvcEsBuf': 1,
 'sceMpegFreeAvcEsBuf': 2, 'sceMpegQueryAtracEsSize': 3, 'sceMpegInitAu': 3, 'sceMpegGetAvcAu': 4, 'sceMpegGetAtracAu': 4,
 'sceMpegFlushAllStream': 1, 'sceMpegAvcDecode': 5, 'sceMpegAvcDecodeStop': 4, 'sceMpegAvcDecodeFlush': 1, 'sceMpegAtracDecode': 4,
 'sceMpegRingbufferQueryMemSize': 1, 'sceMpegRingbufferConstruct': 6, 'sceMpegRingbufferDestruct': 1, 'sceMpegRingbufferPut': 3,
 'sceMpegRingbufferAvailableSize': 1,
 # sceRtc, sceUtility, scePower
 'sceRtcGetCurrentTick': 1, 'sceRtcGetCurrentClock': 2, 'sceRtcGetTick': 2,
 'sceUtilityNetconfInitStart': 1, 'sceUtilityNetconfShutdownStart': 0, 'sceUtilityNetconfUpdate': 1, 'sceUtilityNetconfGetStatus': 0,
 'sceUtilitySavedataInitStart': 1, 'sceUtilitySavedataShutdownStart': 0, 'sceUtilitySavedataUpdate': 1, 'sceUtilitySavedataGetStatus': 0,
 'sceUtilityMsgDialogInitStart': 1, 'sceUtilityMsgDialogShutdownStart': 0, 'sceUtilityMsgDialogUpdate': 1, 'sceUtilityMsgDialogGetStatus': 0,
 'sceUtilityGetSystemParamInt': 2, 'sceUtilityGetSystemParamString': 3, 'scePowerRegisterCallback': 2,
}
def main():
    tsv = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'tools', 'imports.tsv')
    prof = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'infected_profile.json')
    table = {}
    missing = []
    for line in open(tsv):
        addr, lib, nid, name = line.rstrip('\n').split('\t')
        if name in A: table['0x%08X' % int(addr, 16)] = {'name': '%s::%s' % (lib, name), 'gpr': A[name]}
        else: missing.append(name)
    if missing: sys.exit('no arity for: ' + ', '.join(missing))
    p = json.load(open(prof))
    p['import_arity'] = table
    with open(prof, 'w') as f: json.dump(p, f, indent=2); f.write('\n')
    print('wrote %d import arities into %s' % (len(table), prof))
main()
