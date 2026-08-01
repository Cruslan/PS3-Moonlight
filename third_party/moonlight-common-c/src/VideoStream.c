#include "Limelight-internal.h"

#define FIRST_FRAME_MAX 1500
#define FIRST_FRAME_TIMEOUT_SEC 10

#define FIRST_FRAME_PORT 47996

static RTP_VIDEO_QUEUE rtpQueue;

static SOCKET rtpSocket = INVALID_SOCKET;
static SOCKET firstFrameSocket = INVALID_SOCKET;

static PPLT_CRYPTO_CONTEXT decryptionCtx;

static PLT_THREAD udpPingThread;
static PLT_THREAD receiveThread;
static PLT_THREAD decoderThread;

static bool receivedDataFromPeer;
static uint64_t firstDataTimeMs;
static bool receivedFullFrame;

// We can't request an IDR frame until the depacketizer knows
// that a packet was lost. This timeout bounds the time that
// the RTP queue will wait for missing/reordered packets.
// PS3's tiny kernel UDP buffer causes late packet delivery.
// On real hardware (LAN), 15ms is enough to catch reordered packets
// while keeping video latency low.  Higher values increase recovery
// time after packet loss because the depacketizer waits longer before
// requesting an IDR frame.
#ifdef __PPU__
#define RTP_QUEUE_DELAY 15
#else
#define RTP_QUEUE_DELAY 10
#endif

// This is the desired number of video packets that can be
// stored in the socket's receive buffer. 2048 is chosen
// because it should be large enough for all reasonable
// frame sizes (probably 2 or 3 frames) without using too
// much kernel memory with larger packet sizes. It also
// can smooth over transient pauses in network traffic
// and subsequent packet/frame bursts that follow.
#define RTP_RECV_PACKETS_BUFFERED 2048

// Initialize the video stream
void initializeVideoStream(void) {
    initializeVideoDepacketizer(StreamConfig.packetSize);
    RtpvInitializeQueue(&rtpQueue);
    decryptionCtx = PltCreateCryptoContext();
    receivedDataFromPeer = false;
    firstDataTimeMs = 0;
    receivedFullFrame = false;
}

// Clean up the video stream
void destroyVideoStream(void) {
    PltDestroyCryptoContext(decryptionCtx);
    destroyVideoDepacketizer();
    RtpvCleanupQueue(&rtpQueue);
}

// UDP Ping proc
static void VideoPingThreadProc(void* context) {
    char legacyPingData[] = { 0x50, 0x49, 0x4E, 0x47 };
    LC_SOCKADDR saddr;

    LC_ASSERT(VideoPortNumber != 0);

    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    SET_PORT(&saddr, VideoPortNumber);

    // We do not check for errors here. Socket errors will be handled
    // on the read-side in ReceiveThreadProc(). This avoids potential
    // issues related to receiving ICMP port unreachable messages due
    // to sending a packet prior to the host PC binding to that port.
    int pingCount = 0;
    while (!PltIsThreadInterrupted(&udpPingThread)) {
        if (VideoPingPayload.payload[0] != 0) {
            pingCount++;
            VideoPingPayload.sequenceNumber = BE32(pingCount);

            sendto(rtpSocket, (char*)&VideoPingPayload, sizeof(VideoPingPayload), 0, (struct sockaddr*)&saddr, AddrLen);
        }
        else {
            sendto(rtpSocket, legacyPingData, sizeof(legacyPingData), 0, (struct sockaddr*)&saddr, AddrLen);
        }

        PltSleepMsInterruptible(&udpPingThread, 500);
    }
}

// Receive thread proc
static void VideoReceiveThreadProc(void* context) {
    int err;
    int bufferSize, receiveSize, decryptedSize, minSize;
    char* buffer;
    char* encryptedBuffer;
    int queueStatus;
    bool useSelect;
    int waitingForVideoMs;
    bool encrypted;

    encrypted = !!(EncryptionFeaturesEnabled & SS_ENC_VIDEO);
    decryptedSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    minSize = sizeof(RTP_PACKET) + ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
    receiveSize = decryptedSize + ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
    bufferSize = decryptedSize + sizeof(RTPV_QUEUE_ENTRY);
    buffer = NULL;

    if (setNonFatalRecvTimeoutMs(rtpSocket, UDP_RECV_POLL_TIMEOUT_MS) < 0) {
        // SO_RCVTIMEO failed, so use select() to wait
        useSelect = true;
    }
    else {
        // SO_RCVTIMEO timeout set for recv()
        useSelect = false;
    }

    // Allocate a staging buffer to use for each received packet
    if (encrypted) {
        encryptedBuffer = (char*)malloc(receiveSize);
        if (encryptedBuffer == NULL) {
            Limelog("Video Receive: malloc() failed\n");
            ListenerCallbacks.connectionTerminated(-1);
            return;
        }
    }
    else {
        encryptedBuffer = NULL;
    }

    waitingForVideoMs = 0;
    
    // Pre-allocate a batch buffer.  On PS3 real hardware the smaller batch
    // reduces time spent in the drain loop, giving the main thread more
    // opportunities to call vdec_poll() and avoid VDEC back-pressure.
    #ifdef __PPU__
    #define VIDEO_BATCH_SIZE 16
    #define VIDEO_MAX_PACKET_SIZE 1500
    #else
    #define VIDEO_BATCH_SIZE 32
    #define VIDEO_MAX_PACKET_SIZE 2048
    #endif
    static char batch_data[VIDEO_BATCH_SIZE][VIDEO_MAX_PACKET_SIZE];
    static int  batch_lens[VIDEO_BATCH_SIZE];

#ifdef __PPU__
    // PS3 network diagnostics: track packets received and transient errors
    static int ps3_net_pkts_total = 0;
    static uint64_t ps3_net_last_log = 0;
    extern volatile int ps3_enobufs_count; // defined in PlatformSockets.c
#endif

    while (!PltIsThreadInterrupted(&receiveThread)) {
        int batchCount = 0;

        // Phase 1: Drain Phase - Empty the kernel socket buffer into our local batch
        // as fast as the PPU can call recv(). We avoid ALL processing here.
        for (batchCount = 0; batchCount < VIDEO_BATCH_SIZE; batchCount++) {
            err = recvUdpSocket(rtpSocket,
                                batch_data[batchCount],
                                receiveSize,
                                (batchCount == 0) ? useSelect : false);
            
            if (err < 0) {
                if (batchCount > 0 && (LastSocketError() == EWOULDBLOCK || LastSocketError() == EAGAIN)) {
                    break;
                }
                Limelog("Video Receive: recvUdpSocket() failed: %d\n", (int)LastSocketError());
                ListenerCallbacks.connectionTerminated(LastSocketFail());
                goto Exit;
            }
            else if (err == 0) {
                if (batchCount > 0) break; 
                
                if (!receivedDataFromPeer) {
                    waitingForVideoMs += UDP_RECV_POLL_TIMEOUT_MS;
                    if (waitingForVideoMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                        Limelog("Terminating connection due to lack of video traffic\n");
                        ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_TRAFFIC);
                        goto Exit;
                    }
                }
                break;
            }
            batch_lens[batchCount] = err;
        }

        // Phase 2: Process Phase - Now that the network buffer is safe, 
        // perform the heavy lifting (Decryption, Memory Allocation, Queueing).
        for (int i = 0; i < batchCount; i++) {
            int current_len = batch_lens[i];
            char* current_raw = batch_data[i];

            // Successfully received a packet
            if (!receivedDataFromPeer) {
                receivedDataFromPeer = true;
                Limelog("Received first video packet after %d ms\n", waitingForVideoMs);
                firstDataTimeMs = PltGetMillis();
            }

#ifndef LC_FUZZING
            if (!receivedFullFrame) {
                if (PltGetMillis() - firstDataTimeMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                    Limelog("Terminating connection due to lack of a successful video frame\n");
                    ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_FRAME);
                    goto Exit;
                }
            }
#endif

            if (current_len < minSize) {
                continue;
            }

            // Ensure we have a buffer to decrypt into
            if (buffer == NULL) {
                buffer = (char*)malloc(bufferSize);
                if (buffer == NULL) {
                    Limelog("Video Receive: malloc() failed\n");
                    ListenerCallbacks.connectionTerminated(-1);
                    goto Exit;
                }
            }

            // Decrypt the packet if encryption is enabled
            if (encrypted) {
                PENC_VIDEO_HEADER encHeader = (PENC_VIDEO_HEADER)current_raw;

                if (encHeader->frameNumber && LE32(encHeader->frameNumber) < RtpvGetCurrentFrameNumber(&rtpQueue)) {
                    continue;
                }

                if (!PltDecryptMessage(decryptionCtx, ALGORITHM_AES_GCM, 0,
                                    (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                                    encHeader->iv, sizeof(encHeader->iv),
                                    encHeader->tag, sizeof(encHeader->tag),
                                    ((unsigned char*)(encHeader + 1)), current_len - sizeof(ENC_VIDEO_HEADER),
                                    (unsigned char*)buffer, &current_len)) {
                    Limelog("Failed to decrypt video packet!\n");
                    continue;
                }
            } else {
                // If not encrypted, copy from batch to Moonlight-owned buffer
                memcpy(buffer, current_raw, current_len);
            }

            // Convert fields to host byte-order
            PRTP_PACKET packet = (PRTP_PACKET)&buffer[0];
            packet->sequenceNumber = BE16(packet->sequenceNumber);
            packet->timestamp = BE32(packet->timestamp);
            packet->ssrc = BE32(packet->ssrc);

            queueStatus = RtpvAddPacket(&rtpQueue, packet, current_len, (PRTPV_QUEUE_ENTRY)&buffer[decryptedSize]);

            if (queueStatus == RTPF_RET_QUEUED) {
                // The queue takes ownership of the buffer
                buffer = NULL;
            }
        }

#ifdef __PPU__
        // Update PS3 network stats and log periodically
        ps3_net_pkts_total += batchCount;
        {
            uint64_t now = PltGetMillis();
            if (ps3_net_last_log == 0) ps3_net_last_log = now;
            if (now - ps3_net_last_log >= 5000) {
                Limelog("[PS3-NET] video pkts=%d enobufs=%d batch=%d (5s interval)\n",
                        ps3_net_pkts_total, ps3_enobufs_count, batchCount);
                ps3_net_pkts_total = 0;
                ps3_enobufs_count = 0;
                ps3_net_last_log = now;
            }
        }
#endif
    }

Exit:

    if (buffer != NULL) {
        free(buffer);
    }

    if (encryptedBuffer != NULL) {
        free(encryptedBuffer);
    }
}

void notifyKeyFrameReceived(void) {
    // Remember that we got a full frame successfully
    receivedFullFrame = true;
}

// Decoder thread proc
static void VideoDecoderThreadProc(void* context) {
    while (!PltIsThreadInterrupted(&decoderThread)) {
        VIDEO_FRAME_HANDLE frameHandle;
        PDECODE_UNIT decodeUnit;

        if (!LiWaitForNextVideoFrame(&frameHandle, &decodeUnit)) {
            return;
        }

        LiCompleteVideoFrame(frameHandle, VideoCallbacks.submitDecodeUnit(decodeUnit));
    }
}

// Read the first frame of the video stream
int readFirstFrame(void) {
    // All that matters is that we close this socket.
    // This starts the flow of video on Gen 3 servers.

    closeSocket(firstFrameSocket);
    firstFrameSocket = INVALID_SOCKET;

    return 0;
}

// Terminate the video stream
void stopVideoStream(void) {
    if (!receivedDataFromPeer) {
        Limelog("No video traffic was ever received from the host!\n");
    }

    VideoCallbacks.stop();

    // Wake up client code that may be waiting on the decode unit queue
    stopVideoDepacketizer();

    PltInterruptThread(&udpPingThread);
    PltInterruptThread(&receiveThread);
    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        PltInterruptThread(&decoderThread);
    }

    if (firstFrameSocket != INVALID_SOCKET) {
        shutdownTcpSocket(firstFrameSocket);
    }

    PltJoinThread(&udpPingThread);
    PltJoinThread(&receiveThread);
    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        PltJoinThread(&decoderThread);
    }

    if (firstFrameSocket != INVALID_SOCKET) {
        closeSocket(firstFrameSocket);
        firstFrameSocket = INVALID_SOCKET;
    }
    if (rtpSocket != INVALID_SOCKET) {
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
    }

    VideoCallbacks.cleanup();
}

// Start the video stream
int startVideoStream(void* rendererContext, int drFlags) {
    int err;

    firstFrameSocket = INVALID_SOCKET;

    // This must be called before the decoder thread starts submitting
    // decode units
    LC_ASSERT(NegotiatedVideoFormat != 0);
    err = VideoCallbacks.setup(NegotiatedVideoFormat, StreamConfig.width,
        StreamConfig.height, StreamConfig.fps, rendererContext, drFlags);
    if (err != 0) {
        return err;
    }

#ifdef __PPU__
    // PS3 libnet has a very small default UDP recv buffer (~8KB) and rejects
    // SO_RCVBUF values above ~64KB. It also requires even-numbered sizes.
    // Request 64KB (65536) explicitly; bindUdpSocket()'s fallback loop
    // will step down in RCV_BUFFER_SIZE_STEP (4096) increments if needed.
    // Previous code computed 64*1460=93KB which was rejected entirely,
    // leaving the socket at the unusable 8KB default.
    {
        int videoRcvBufSize = 64 * 1024; // 65536 — even, within PS3 limit
        rtpSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen,
                                  videoRcvBufSize,
                                  SOCK_QOS_TYPE_VIDEO);
    }
#else
    rtpSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen,
                              RTP_RECV_PACKETS_BUFFERED * (StreamConfig.packetSize + MAX_RTP_HEADER_SIZE),
                              SOCK_QOS_TYPE_VIDEO);
#endif
    if (rtpSocket == INVALID_SOCKET) {
        VideoCallbacks.cleanup();
        return LastSocketError();
    }

    VideoCallbacks.start();

    err = PltCreateThread("VideoRecv", VideoReceiveThreadProc, NULL, &receiveThread);
    if (err != 0) {
        VideoCallbacks.stop();
        closeSocket(rtpSocket);
        VideoCallbacks.cleanup();
        return err;
    }

    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        err = PltCreateThread("VideoDec", VideoDecoderThreadProc, NULL, &decoderThread);
        if (err != 0) {
            VideoCallbacks.stop();
            PltInterruptThread(&receiveThread);
            PltJoinThread(&receiveThread);
            closeSocket(rtpSocket);
            VideoCallbacks.cleanup();
            return err;
        }
    }

    if (AppVersionQuad[0] == 3) {
        // Connect this socket to open port 47998 for our ping thread
        firstFrameSocket = connectTcpSocket(&RemoteAddr, AddrLen,
                                            FIRST_FRAME_PORT, FIRST_FRAME_TIMEOUT_SEC);
        if (firstFrameSocket == INVALID_SOCKET) {
            VideoCallbacks.stop();
            stopVideoDepacketizer();
            PltInterruptThread(&receiveThread);
            if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
                PltInterruptThread(&decoderThread);
            }
            PltJoinThread(&receiveThread);
            if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
                PltJoinThread(&decoderThread);
            }
            closeSocket(rtpSocket);
            VideoCallbacks.cleanup();
            return LastSocketError();
        }
    }

    // Start pinging before reading the first frame so GFE knows where
    // to send UDP data
    err = PltCreateThread("VideoPing", VideoPingThreadProc, NULL, &udpPingThread);
    if (err != 0) {
        VideoCallbacks.stop();
        stopVideoDepacketizer();
        PltInterruptThread(&receiveThread);
        if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
            PltInterruptThread(&decoderThread);
        }
        PltJoinThread(&receiveThread);
        if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
            PltJoinThread(&decoderThread);
        }
        closeSocket(rtpSocket);
        if (firstFrameSocket != INVALID_SOCKET) {
            closeSocket(firstFrameSocket);
            firstFrameSocket = INVALID_SOCKET;
        }
        VideoCallbacks.cleanup();
        return err;
    }

    if (AppVersionQuad[0] == 3) {
        // Read the first frame to start the flow of video
        err = readFirstFrame();
        if (err != 0) {
            stopVideoStream();
            return err;
        }
    }

    return 0;
}

const RTP_VIDEO_STATS* LiGetRTPVideoStats(void) {
    return &rtpQueue.stats;
}
