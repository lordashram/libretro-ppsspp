// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.
//
#include "base/mutex.h"

#include "Globals.h" // only for clamp_s16
#include "Common/CommonTypes.h"
#include "Common/ChunkFile.h"
#include "Common/Atomics.h"

#include "Core/CoreTiming.h"
#include "Core/MemMap.h"
#include "Core/Host.h"
#include "Core/Config.h"
#include "Core/HLE/__sceAudio.h"
#include "Core/HLE/sceAudio.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceKernelThread.h"

enum latency {
	LOW_LATENCY = 0,
	MEDIUM_LATENCY = 1,
	HIGH_LATENCY = 2,
};

int eventAudioUpdate = -1;
int eventHostAudioUpdate = -1; 
int mixFrequency = 44100;

const int hwSampleRate = 44100;

int hwBlockSize = 64;
int hostAttemptBlockSize = 512;

static int audioIntervalCycles;
static int audioHostIntervalCycles;

#define MIXBUFFER_QUEUE (512 * 16)

#if defined(_M_IX86) || defined(_M_ARM32)
#define GET_PTR(address) (u8*)(Memory::base + ((address) & Memory::MEMVIEW32_MASK))
#else
#define GET_PTR(address) (u8*)(Memory::base + (address))
#endif

static s16 mixBufferQueue[MIXBUFFER_QUEUE];
static int mixBufferHead = 0;
static int mixBufferTail = 0;
static int mixBufferCount = 0; // sacrifice 4 bytes for a simpler implementation. may optimize away in the future.

static void queue_DoState(PointerWrap &p)
{
   int size = MIXBUFFER_QUEUE;
   p.Do(size);
   if (size != MIXBUFFER_QUEUE)
   {
      ERROR_LOG(COMMON, "Savestate failure: Incompatible queue size.");
      return;
   }
   p.DoArray<s16>(mixBufferQueue, MIXBUFFER_QUEUE);
   p.Do(mixBufferHead);
   p.Do(mixBufferTail);
   p.Do(mixBufferCount);
   p.DoMarker("FixedSizeQueueLR");
}

static s32 *mixBuffer;

// High and low watermarks, basically.  For perfect emulation, the correct values are 0 and 1, respectively.
// TODO: Tweak. Hm, there aren't actually even used currently...
static int chanQueueMaxSizeFactor;
static int chanQueueMinSizeFactor;


static inline s16 adjustvolume(s16 sample, int vol) {
#ifdef ARM
	register int r;
	asm volatile("smulwb %0, %1, %2\n\t" \
	             "ssat %0, #16, %0" \
	             : "=r"(r) : "r"(vol), "r"(sample));
	return r;
#else
	return clamp_s16((sample * vol) >> 16);
#endif
}

void hleAudioUpdate(u64 userdata, int cyclesLate)
{
	// Schedule the next cycle first.  __AudioUpdate() may consume cycles.
	CoreTiming::ScheduleEvent(audioIntervalCycles - cyclesLate, eventAudioUpdate, 0);

	__AudioUpdate();
}

void hleHostAudioUpdate(u64 userdata, int cyclesLate)
{
	CoreTiming::ScheduleEvent(audioHostIntervalCycles - cyclesLate, eventHostAudioUpdate, 0);

	// Not all hosts need this call to poke their audio system once in a while, but those that don't
	// can just ignore it.
	host->UpdateSound();
}

void __AudioCPUMHzChange()
{
	audioIntervalCycles = (int)(usToCycles(1000000ULL) * hwBlockSize / hwSampleRate);
	audioHostIntervalCycles = (int)(usToCycles(1000000ULL) * hostAttemptBlockSize / hwSampleRate);
}


void __AudioInit() {
	mixFrequency = 44100;

	switch (g_Config.iAudioLatency) {
	case LOW_LATENCY:
		chanQueueMaxSizeFactor = 1;
		chanQueueMinSizeFactor = 1;
		hwBlockSize = 16;
		hostAttemptBlockSize = 256;
		break;
	case MEDIUM_LATENCY:
		chanQueueMaxSizeFactor = 2;
		chanQueueMinSizeFactor = 1;
		hwBlockSize = 64;
		hostAttemptBlockSize = 512;
		break;
	case HIGH_LATENCY:
		chanQueueMaxSizeFactor = 4;
		chanQueueMinSizeFactor = 2;
		hwBlockSize = 64;
		hostAttemptBlockSize = 512;
		break;

	}

	__AudioCPUMHzChange();

	eventAudioUpdate = CoreTiming::RegisterEvent("AudioUpdate", &hleAudioUpdate);
	eventHostAudioUpdate = CoreTiming::RegisterEvent("AudioUpdateHost", &hleHostAudioUpdate);

	CoreTiming::ScheduleEvent(audioIntervalCycles, eventAudioUpdate, 0);
	CoreTiming::ScheduleEvent(audioHostIntervalCycles, eventHostAudioUpdate, 0);
	for (u32 i = 0; i < PSP_AUDIO_CHANNEL_MAX + 1; i++)
		chans[i].clear();

	mixBuffer = new s32[hwBlockSize * 2];
	memset(mixBuffer, 0, hwBlockSize * 2 * sizeof(s32));

   mixBufferHead = 0;
   mixBufferTail = 0;
   mixBufferCount = 0;

	CoreTiming::RegisterMHzChangeCallback(&__AudioCPUMHzChange);
}

void __AudioDoState(PointerWrap &p)
{
	auto s = p.Section("sceAudio", 1);
	if (!s)
		return;

	p.Do(eventAudioUpdate);
	CoreTiming::RestoreRegisterEvent(eventAudioUpdate, "AudioUpdate", &hleAudioUpdate);
	p.Do(eventHostAudioUpdate);
	CoreTiming::RestoreRegisterEvent(eventHostAudioUpdate, "AudioUpdateHost", &hleHostAudioUpdate);

	p.Do(mixFrequency);

   queue_DoState(p);

	int chanCount = ARRAY_SIZE(chans);
	p.Do(chanCount);
	if (chanCount != ARRAY_SIZE(chans))
	{
		ERROR_LOG(SCEAUDIO, "Savestate failure: different number of audio channels.");
		return;
	}
	for (int i = 0; i < chanCount; ++i)
		chans[i].DoState(p);

	__AudioCPUMHzChange();
}

void __AudioShutdown()
{
	delete [] mixBuffer;

	mixBuffer = 0;

	for (u32 i = 0; i < PSP_AUDIO_CHANNEL_MAX + 1; i++)
		chans[i].clear();
}

u32 __AudioEnqueue(AudioChannel &chan, int chanNum, bool blocking)
{
	u32 ret = chan.sampleCount;

	if (chan.sampleAddress == 0)
   {
      // For some reason, multichannel audio lies and returns the sample count here.
      if (chanNum == PSP_AUDIO_CHANNEL_SRC || chanNum == PSP_AUDIO_CHANNEL_OUTPUT2)
         ret = 0;
   }

	// If there's anything on the queue at all, it should be busy, but we try to be a bit lax.
	//if (chan.sampleQueue.size() > chan.sampleCount * 2 * chanQueueMaxSizeFactor || chan.sampleAddress == 0) {
	if (chan.sampleQueue.size() > 0)
   {
      if (blocking)
      {
         // TODO: Regular multichannel audio seems to block for 64 samples less?  Or enqueue the first 64 sync?
         int blockSamples = (int)chan.sampleQueue.size() / 2 / chanQueueMinSizeFactor;

         if (__KernelIsDispatchEnabled())
         {
            AudioChannelWaitInfo waitInfo = {__KernelGetCurThread(), blockSamples};
            chan.waitingThreads.push_back(waitInfo);
            // Also remember the value to return in the waitValue.
            __KernelWaitCurThread(WAITTYPE_AUDIOCHANNEL, (SceUID)chanNum + 1, ret, 0, false, "blocking audio");
         }
         else // TODO: Maybe we shouldn't take this audio after all?
            ret = SCE_KERNEL_ERROR_CAN_NOT_WAIT;

         // Fall through to the sample queueing, don't want to lose the samples even though
         // we're getting full.  The PSP would enqueue after blocking.
      }
      else // Non-blocking doesn't even enqueue, but it's not commonly used.
         return SCE_ERROR_AUDIO_CHANNEL_BUSY;
   }

	if (chan.sampleAddress == 0)
		return ret;

	int leftVol = chan.leftVolume;
	int rightVol = chan.rightVolume;

	if (leftVol == (1 << 15) && rightVol == (1 << 15) && chan.format == PSP_AUDIO_FORMAT_STEREO && IS_LITTLE_ENDIAN)
   {
      // TODO: Add mono->stereo conversion to this path.

      // Good news: the volume doesn't affect the values at all.
      // We can just do a direct memory copy.
      const u32 totalSamples = chan.sampleCount * (chan.format == PSP_AUDIO_FORMAT_STEREO ? 2 : 1);
      s16 *buf1 = 0, *buf2 = 0;
      size_t sz1, sz2;
      chan.sampleQueue.pushPointers(totalSamples, &buf1, &sz1, &buf2, &sz2);

      memcpy(buf1, GET_PTR(chan.sampleAddress), sz1 * sizeof(s16));
      if (buf2)
         memcpy(buf2, GET_PTR(chan.sampleAddress + (u32)sz1 * sizeof(s16)), sz2 * sizeof(s16));
   }
   else
   {
      // Remember that maximum volume allowed is 0xFFFFF so left shift is no issue.
      // This way we can optimally shift by 16.
      leftVol <<=1;
      rightVol <<=1;

      if (chan.format == PSP_AUDIO_FORMAT_STEREO)
      {
         const u32 totalSamples = chan.sampleCount * 2;

         s16_le *sampleData = (s16_le *)GET_PTR(chan.sampleAddress);

         // Walking a pointer for speed.  But let's make sure we wouldn't trip on an invalid ptr.
         s16 *buf1 = 0, *buf2 = 0;
         size_t sz1, sz2;
         chan.sampleQueue.pushPointers(totalSamples, &buf1, &sz1, &buf2, &sz2);

         // TODO: SSE/NEON (VQDMULH) implementations
         for (u32 i = 0; i < sz1; i += 2)
         {
            buf1[i] = adjustvolume(sampleData[i], leftVol);
            buf1[i + 1] = adjustvolume(sampleData[i + 1], rightVol);
         }
         if (buf2)
         {
            sampleData += sz1;
            for (u32 i = 0; i < sz2; i += 2)
            {
               buf2[i] = adjustvolume(sampleData[i], leftVol);
               buf2[i + 1] = adjustvolume(sampleData[i + 1], rightVol);
            }
         }
      }
      else if (chan.format == PSP_AUDIO_FORMAT_MONO)
      {
         for (u32 i = 0; i < chan.sampleCount; i++)
         {
            // Expand to stereo
            s16 *sample = (s16*)GET_PTR(chan.sampleAddress + 2 * i);
            chan.sampleQueue.push(adjustvolume(*sample, leftVol));
            chan.sampleQueue.push(adjustvolume(*sample, rightVol));
         }
      }
   }
	return ret;
}

inline void __AudioWakeThreads(AudioChannel &chan, int result, int step) {
	u32 error;
	bool wokeThreads = false;
	for (size_t w = 0; w < chan.waitingThreads.size(); ++w) {
		AudioChannelWaitInfo &waitInfo = chan.waitingThreads[w];
		waitInfo.numSamples -= step;

		// If it's done (there will still be samples on queue) and actually still waiting, wake it up.
		u32 waitID = __KernelGetWaitID(waitInfo.threadID, WAITTYPE_AUDIOCHANNEL, error);
		if (waitInfo.numSamples <= 0 && waitID != 0) {
			// DEBUG_LOG(SCEAUDIO, "Woke thread %i for some buffer filling", waitingThread);
			u32 ret = result == 0 ? __KernelGetWaitValue(waitInfo.threadID, error) : SCE_ERROR_AUDIO_CHANNEL_NOT_RESERVED;
			__KernelResumeThreadFromWait(waitInfo.threadID, ret);
			wokeThreads = true;

			chan.waitingThreads.erase(chan.waitingThreads.begin() + w--);
		}
		// This means the thread stopped waiting, so stop trying to wake it.
		else if (waitID == 0)
			chan.waitingThreads.erase(chan.waitingThreads.begin() + w--);
	}

	if (wokeThreads) {
		__KernelReSchedule("audio drain");
	}
}

void __AudioWakeThreads(AudioChannel &chan, int result) {
	__AudioWakeThreads(chan, result, 0x7FFFFFFF);
}

void __AudioSetOutputFrequency(int freq) {
	WARN_LOG(SCEAUDIO, "Switching audio frequency to %i", freq);
	mixFrequency = freq;
}

// Mix samples from the various audio channels into a single sample queue.
// This single sample queue is where __AudioMix should read from. If the sample queue is full, we should
// just sleep the main emulator thread a little.
void __AudioUpdate()
{
	// Audio throttle doesn't really work on the PSP since the mixing intervals are so closely tied
	// to the CPU. Much better to throttle the frame rate on frame display and just throw away audio
	// if the buffer somehow gets full.
   memset(mixBuffer, 0, hwBlockSize * 2 * sizeof(s32));

	for (u32 i = 0; i < PSP_AUDIO_CHANNEL_MAX + 1; i++)
   {
      if (!chans[i].reserved)
         continue;

      __AudioWakeThreads(chans[i], 0, hwBlockSize);

      if (!chans[i].sampleQueue.size())
         continue;

      const s16 *buf1 = 0, *buf2 = 0;
      size_t sz1, sz2;

      chans[i].sampleQueue.popPointers(hwBlockSize * 2, &buf1, &sz1, &buf2, &sz2);

      for (size_t s = 0; s < sz1; s++)
         mixBuffer[s] += buf1[s];
      if (buf2)
      {
         for (size_t s = 0; s < sz2; s++)
            mixBuffer[s + sz1] += buf2[s];
      }
   }

   {
      size_t size = hwBlockSize * 2;
      s16 *dest1 = (s16*)&mixBufferQueue[mixBufferTail];
      s16 *dest2 = (s16*)&mixBufferQueue[0];
      size_t sz1 = MIXBUFFER_QUEUE - mixBufferTail;

      if (mixBufferTail + (int)size < MIXBUFFER_QUEUE)
      {
         sz1 = size;
         mixBufferTail += (int)size;
         if (mixBufferTail == MIXBUFFER_QUEUE)
            mixBufferTail = 0;
      }
      else
      {
         size_t sz2 = mixBufferTail = (int)(size - sz1);
         for (size_t s = 0; s < sz2; s++)
            dest2[s] = clamp_s16(mixBuffer[s + sz1]);
      }
      mixBufferCount += (int)size;

      for (size_t s = 0; s < sz1; s++)
         dest1[s] = clamp_s16(mixBuffer[s]);
   }
}

// numFrames is number of stereo frames.
// This is called from *outside* the emulator thread.
int __AudioMix(short *outstereo, int numFrames)
{
	const s16 *src1 = (s16*)&mixBufferQueue[mixBufferHead];
   const s16 *src2 = (s16*)&mixBufferQueue[0];
   size_t size = mixBufferCount;
	size_t sz1 = MIXBUFFER_QUEUE - mixBufferHead;
   size_t sz2 = 0;

   if (mixBufferHead + size < MIXBUFFER_QUEUE)
   {
      sz1 = size;
      mixBufferHead += (int)size;
      if (mixBufferHead == MIXBUFFER_QUEUE)
         mixBufferHead = 0;
   }
   else
   {
      mixBufferHead = (int)(size - sz1);
      sz2 = mixBufferHead;
      memcpy(outstereo + sz1, src2, sz2 * sizeof(s16));
   }
   memcpy(outstereo, src1, sz1 * sizeof(s16));
   mixBufferCount -= (int)size;

	return ((sz1 + sz2) / 2);
}
