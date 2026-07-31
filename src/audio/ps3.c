#include "ps3.h"
#include <stdio.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include <sys/event_queue.h>
#include <sys/mutex.h>
#include <string.h>
#include <opus_multistream.h>

/* Number of audio channels for stereo output */
#define AUDIO_CHANNELS 2

/* Size of the temporary buffer used to decode Opus frame.
   Opus supports up to 120ms frame sizes, which translates to
   5760 samples per channel at 48kHz sample rate. */
#define MAX_OPUS_FRAME_SIZE 5760

/* Circular ring buffer size in floats.
   A buffer of 32768 floats can store 16384 stereo samples.
   At 48000 Hz, this represents approximately 341 milliseconds
   of audio. This provides enough buffering to handle system scheduling
   jitter while avoiding excessive audio latency. */
#define RING_BUFFER_SIZE 32768

/* Circular ring buffer to bridge the Opus decode callback and the audio playback thread */
static float audio_ring_buffer[RING_BUFFER_SIZE];
static int rb_write_idx = 0;
static int rb_read_idx = 0;
static sys_mutex_t rb_mutex;

/* Thread and audio system handles */
static int active_audio_thread = 0;
static u32 audio_port;
static audioPortConfig audio_cfg;
static sys_event_queue_t audio_queue;
static sys_ipc_key_t audio_key;
static sys_ppu_thread_t audio_thread;

/* Opus Multistream Decoder instance pointer */
static OpusMSDecoder *opus_decoder = NULL;

/* Decode buffer for Opus float PCM decoding.
   Needs to hold stereo samples (channels * frame size). */
static float decode_buffer[MAX_OPUS_FRAME_SIZE * AUDIO_CHANNELS];

/* Decoded audio packets counter for visual debug overlay */
static u32 audio_decoded_packets_count = 0;

/**
 * Write float audio samples into the circular ring buffer.
 * If the ring buffer runs out of space, we prioritize real-time low latency
 * by advancing the read pointer, thus discarding the oldest samples to make room
 * for the newest audio packets.
 */
static void rb_write(const float* data, int num_floats) {
    sysMutexLock(rb_mutex, 0);
    
    int free_space = (rb_read_idx - rb_write_idx - 1 + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
    
    /* Check if we have enough space in the buffer.
       If the incoming data size exceeds free space, we discard the oldest data.
       This is critical in game streaming because real-time audio synchronization
       is more important than preserving stale audio blocks. */
    if (free_space < num_floats) {
        int needed = num_floats - free_space;
        rb_read_idx = (rb_read_idx + needed) % RING_BUFFER_SIZE;
    }
    
    for (int i = 0; i < num_floats; i++) {
        audio_ring_buffer[rb_write_idx] = data[i];
        rb_write_idx = (rb_write_idx + 1) % RING_BUFFER_SIZE;
    }
    
    sysMutexUnlock(rb_mutex);
}

/**
 * Read float audio samples from the circular ring buffer.
 * If there is not enough audio data to fill the request, it returns 0.
 * To keep audio latency minimal, if the buffer accumulates too much backlog
 * (more than 4096 floats or ~42ms of audio), we skip ahead to the latest 1024 floats.
 */
static int rb_read(float* data, int num_floats) {
    sysMutexLock(rb_mutex, 0);
    
    int available = (rb_write_idx - rb_read_idx + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
    if (available < num_floats) {
        sysMutexUnlock(rb_mutex);
        return 0; /* Not enough data (underflow) */
    }
    
    for (int i = 0; i < num_floats; i++) {
        data[i] = audio_ring_buffer[rb_read_idx];
        rb_read_idx = (rb_read_idx + 1) % RING_BUFFER_SIZE;
    }
    
    /* Prevent buffer bloat: if too many samples accumulate in the ring buffer
       due to scheduling jitter or decode bursts, we skip older samples.
       4096 floats represent 2048 stereo samples (approx 42ms).
       If we exceed this threshold, we advance the read pointer to retain only
       the most recent 1024 floats (512 stereo samples, or ~10.6ms). */
    int remaining = (rb_write_idx - rb_read_idx + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
    if (remaining > 4096) {
        rb_read_idx = (rb_write_idx - 1024 + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
    }
    
    sysMutexUnlock(rb_mutex);
    return 1;
}

/**
 * High-priority audio playback thread loop.
 * It waits for the PS3 audio notification queue to signal that a block in the
 * circular audio hardware buffer is free, then writes the next decoded block of floats.
 */
static void audio_loop(void* arg) {
    sys_event_t event;
    u32 current_block = 0;
    float temp_block[AUDIO_CHANNELS * AUDIO_BLOCK_SAMPLES];
    
    while (active_audio_thread) {
        /* Timeout is 50,000 microseconds (50ms) to ensure thread can exit quickly on shutdown */
        if (sysEventQueueReceive(audio_queue, &event, 50000) == 0) {
            /* Compute the target hardware buffer address for the current block */
            float* buffer_addr = (float*)((u64)audio_cfg.audioDataStart + 
                                (current_block * AUDIO_CHANNELS * AUDIO_BLOCK_SAMPLES * sizeof(float)));
            
            /* Attempt to pull stereo samples from the ring buffer.
               Each block size is channels (2) * samples per block (256) = 512 floats. */
            if (rb_read(temp_block, AUDIO_CHANNELS * AUDIO_BLOCK_SAMPLES)) {
                memcpy(buffer_addr, temp_block, AUDIO_CHANNELS * AUDIO_BLOCK_SAMPLES * sizeof(float));
            } else {
                /* Underflow occurred. Write silence to prevent static noise */
                memset(buffer_addr, 0, AUDIO_CHANNELS * AUDIO_BLOCK_SAMPLES * sizeof(float));
            }
            
            /* Advance to the next block in the hardware ring buffer */
            current_block = (current_block + 1) % audio_cfg.numBlocks;
        }
    }
    sysThreadExit(0);
}

/**
 * Clean up the audio renderer, stopping threads, closing ports, and freeing resources.
 */
static void ps3_renderer_cleanup() {
    active_audio_thread = 0;
    
    /* Wait for the playback thread to terminate cleanly */
    u64 retval;
    sysThreadJoin(audio_thread, &retval);
    
    /* Stop audio port transmission */
    audioPortStop(audio_port);
    
    /* Unregister the notification event queue */
    audioRemoveNotifyEventQueue(audio_key);
    
    /* Close port and clean up event queue and mutex */
    audioPortClose(audio_port);
    sysEventQueueDestroy(audio_queue, 0);
    sysMutexDestroy(rb_mutex);
    
    /* Free Opus decoder instance */
    if (opus_decoder) {
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
    }
    
    audioQuit();
}

/**
 * Initialize the PS3 audio output system, notify queue, ring buffer mutex, and the Opus decoder.
 */
static int ps3_renderer_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* audioContext, int arFlags) {
    if (audioInit() != 0) return -1;
    
    /* Reset circular buffer indices and packets counter */
    rb_write_idx = 0;
    rb_read_idx = 0;
    audio_decoded_packets_count = 0;
    
    /* Create mutex for ring buffer synchronization */
    sys_mutex_attr_t attr;
    sysMutexAttrInitialize(attr);
    if (sysMutexCreate(&rb_mutex, &attr) != 0) {
        audioQuit();
        return -1;
    }
    
    /* Initialize Opus Multistream Decoder using parameters provided by Moonlight/Sunshine */
    int error = OPUS_OK;
    opus_decoder = opus_multistream_decoder_create(
        opusConfig->sampleRate,
        opusConfig->channelCount,
        opusConfig->streams,
        opusConfig->coupledStreams,
        opusConfig->mapping,
        &error
    );
    if (!opus_decoder || error != OPUS_OK) {
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }
    
    /* Set up stereo playback with 8 blocks of 256 samples */
    audioPortParam param;
    param.numChannels = AUDIO_PORT_2CH;
    param.numBlocks = AUDIO_BLOCK_8;
    param.attrib = 0;
    param.level = 1.0f;
    
    if (audioPortOpen(&param, &audio_port) != 0) {
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }
    
    if (audioGetPortConfig(audio_port, &audio_cfg) != 0) {
        audioPortClose(audio_port);
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }
    
    /* Create event queue for block-completion notifications from the audio hardware */
    if (audioCreateNotifyEventQueue(&audio_queue, &audio_key) != 0) {
        audioPortClose(audio_port);
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }
    
    if (audioSetNotifyEventQueue(audio_key) != 0) {
        audioRemoveNotifyEventQueue(audio_key);
        sysEventQueueDestroy(audio_queue, 0);
        audioPortClose(audio_port);
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }
    
    /* Create and start the playback thread with high priority (100) */
    active_audio_thread = 1;
    if (sysThreadCreate(&audio_thread, audio_loop, 0, 100, 0x4000, THREAD_JOINABLE, "Audio Thread") != 0) {
        active_audio_thread = 0;
        audioRemoveNotifyEventQueue(audio_key);
        sysEventQueueDestroy(audio_queue, 0);
        audioPortClose(audio_port);
        opus_multistream_decoder_destroy(opus_decoder);
        opus_decoder = NULL;
        sysMutexDestroy(rb_mutex);
        audioQuit();
        return -1;
    }
    
    /* Enable audio playback on the hardware port */
    audioPortStart(audio_port);
    
    return 0;
}

/**
 * Decode incoming Opus packet into float PCM samples and push them into the ring buffer.
 */
static void ps3_renderer_decode_and_play_sample(char* data, int length) {
    if (!opus_decoder) return;
    
    /* Decode the raw Opus multistream packet directly to float PCM */
    int decoded_samples = opus_multistream_decode_float(
        opus_decoder,
        (data && length > 0) ? (const unsigned char*)data : NULL,
        length,
        decode_buffer,
        MAX_OPUS_FRAME_SIZE,
        0
    );
    if (decoded_samples < 0) {
        /* Failed to decode the Opus frame */
        return;
    }
    
    /* Push the stereo samples (decoded_samples * channels) into the playback FIFO buffer */
    rb_write(decode_buffer, decoded_samples * AUDIO_CHANNELS);
    audio_decoded_packets_count++;
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_ps3 = {
  .init = ps3_renderer_init,
  .cleanup = ps3_renderer_cleanup,
  .decodeAndPlaySample = ps3_renderer_decode_and_play_sample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT,
};

void ps3audio_start() {
  /* Helper function matching interface. Thread starting is handled in init */
}

void ps3audio_stop() {
  /* Helper function matching interface. Thread stopping is handled in cleanup */
}

unsigned int ps3audio_get_decoded_packets() {
    return audio_decoded_packets_count;
}
