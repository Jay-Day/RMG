/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "RollbackNetplay.hpp"
#include "Error.hpp"
#include "Library.hpp"

#include <chrono>
#include <thread>
#include <mutex>
#include <cstring>
#include <vector>
#include <iostream>

// Constants for GGPO flags not defined in the GGPO header
#define GGPO_RUNFLAG_ROLLBACK 1

// Forward declaration
class RollbackNetplayImpl;

// Implementation class to hide GGPO details
class RollbackNetplayImpl 
{
public:
    RollbackNetplayImpl() : 
        ggpoSession(nullptr),
        localPlayer(0),
        maxPlayers(0),
        initialized(false),
        currentFrame(0),
        currentInputSequence(0),
        lastSavedFrameInputSequence(0),
        saveStateFn(nullptr),
        loadStateFn(nullptr),
        freeStateFn(nullptr),
        advanceFrameFn(nullptr)
    {
        // Initialize metrics directly instead of calling Reset()
        metrics.rollbackFrames = 0;
        metrics.totalRollbacks = 0;
        metrics.predictedFrames = 0;
        metrics.maxRollbackFrames = 0;
        metrics.avgRollbackFrames = 0.0f;
        metrics.pingMs = 0;
        metrics.remoteFrameAdvantage = 0;
    }

    ~RollbackNetplayImpl()
    {
        Shutdown();
    }

    // GGPO callbacks
    static bool BeginGame_Callback(const char* game)
    {
        // Simply return true to allow the game to start
        return true;
    }

    static bool SaveGameState_Callback(unsigned char** buffer, int* len, int* checksum, int frame)
    {
        auto instance = GetInstance();
        if (!instance || !instance->saveStateFn)
            return false;
            
        // Track the current input sequence
        instance->lastSavedFrameInputSequence = instance->currentInputSequence;
        
        return instance->saveStateFn((void**)buffer, len, checksum, frame);
    }

    static bool LoadGameState_Callback(unsigned char* buffer, int len)
    {
        auto instance = GetInstance();
        if (!instance || !instance->loadStateFn)
            return false;
            
        return instance->loadStateFn(buffer, len);
    }

    static void FreeBuffer_Callback(void* buffer)
    {
        auto instance = GetInstance();
        if (instance && instance->freeStateFn)
            instance->freeStateFn(buffer);
    }

    static bool AdvanceFrame_Callback(int)
    {
        auto instance = GetInstance();
        if (!instance || !instance->advanceFrameFn)
            return false;
            
        // Increment frame counter
        instance->currentFrame++;
        
        return instance->advanceFrameFn();
    }

    // Add a flag for rollback event detection
    bool rollbackJustOccurred = false;

    // Add a method to check if rollback just occurred
    bool RollbackJustOccurred() {
        bool result = rollbackJustOccurred;
        rollbackJustOccurred = false;
        return result;
    }

    // Modify the OnEvent callback to detect rollbacks
    static bool OnEvent_Callback(GGPOEvent* event) {
        auto instance = GetInstance();
        if (!instance) {
            return true;
        }
        
        // Update metrics based on the event
        instance->UpdateMetrics(event);
        
        // Check for events that might indicate rollback
        // Note: In the current version of GGPO, there's no direct 'running.flags' to check for rollback
        // Instead, we'll rely on frame discrepancies during various events to detect rollback
        if (event->code == GGPO_EVENTCODE_TIMESYNC) {
            // TimeSyncing can indicate a potential rollback when frames_ahead > 0
            if (event->u.timesync.frames_ahead > 0) {
                // We're likely rolling back due to time sync issues
                int rollbackFrames = event->u.timesync.frames_ahead;
                
                if (rollbackFrames > 0) {
                    // Set the rollback occurred flag
                    instance->rollbackJustOccurred = true;
                    
                    instance->metrics.rollbackFrames += rollbackFrames;
                    instance->metrics.totalRollbacks++;
                    
                    // Update max rollback frames
                    if (rollbackFrames > instance->metrics.maxRollbackFrames) {
                        instance->metrics.maxRollbackFrames = rollbackFrames;
                    }
                    
                    // Update average
                    instance->metrics.avgRollbackFrames = 
                        (float)instance->metrics.rollbackFrames / instance->metrics.totalRollbacks;
                    
                    std::cout << "ROLLBACK: " << rollbackFrames << " frames (max: " 
                              << instance->metrics.maxRollbackFrames 
                              << ", avg: " << instance->metrics.avgRollbackFrames << ")" << std::endl;
                }
            }
        }
        
        return true;
    }

    bool Initialize(const std::string& address, int port, int player, int numPlayers, int frameDelay)
    {
        if (initialized) {
            return true;
        }

        // Set up the instance for callbacks
        if (instancePtr) {
            CoreSetError("RollbackNetplay: Another instance is already active");
            return false;
        }
        instancePtr = this;

        // Store parameters
        localPlayer = player;
        maxPlayers = numPlayers;

        // Set up GGPO callbacks
        GGPOSessionCallbacks callbacks;
        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.begin_game = BeginGame_Callback;
        callbacks.save_game_state = SaveGameState_Callback;
        callbacks.load_game_state = LoadGameState_Callback;
        callbacks.free_buffer = FreeBuffer_Callback;
        callbacks.advance_frame = AdvanceFrame_Callback;
        callbacks.on_event = OnEvent_Callback;

        // Start the session
        GGPOErrorCode result = ggpo_start_session(
            &ggpoSession,
            &callbacks,
            "mupen64plus",
            maxPlayers,
            ROLLBACK_INPUT_BYTES,
            port);

        if (result != GGPO_OK) {
            CoreSetError("RollbackNetplay: Failed to start GGPO session");
            instancePtr = nullptr;
            return false;
        }

        // Set synchronization parameters
        ggpo_set_disconnect_timeout(ggpoSession, 3000);
        ggpo_set_disconnect_notify_start(ggpoSession, 1000);

        // Add players
        for (int i = 0; i < maxPlayers; i++) {
            GGPOPlayer ggpoPlayer;
            memset(&ggpoPlayer, 0, sizeof(ggpoPlayer));
            ggpoPlayer.size = sizeof(GGPOPlayer);
            ggpoPlayer.player_num = i + 1;

            if (i + 1 == localPlayer) {
                // Local player
                ggpoPlayer.type = GGPO_PLAYERTYPE_LOCAL;
                result = ggpo_add_player(ggpoSession, &ggpoPlayer, &localPlayerHandle);
            } else {
                // Remote player
                ggpoPlayer.type = GGPO_PLAYERTYPE_REMOTE;
                strncpy(ggpoPlayer.u.remote.ip_address, address.c_str(), sizeof(ggpoPlayer.u.remote.ip_address) - 1);
                ggpoPlayer.u.remote.port = port;
                result = ggpo_add_player(ggpoSession, &ggpoPlayer, &remotePlayerHandles[i]);
            }

            if (result != GGPO_OK) {
                std::string error = "RollbackNetplay: Failed to add player " + std::to_string(i + 1);
                CoreSetError(error);
                ggpo_close_session(ggpoSession);
                ggpoSession = nullptr;
                instancePtr = nullptr;
                return false;
            }
        }

        // Set frame delay for local player to reduce perceived input lag
        result = ggpo_set_frame_delay(ggpoSession, localPlayerHandle, frameDelay);
        if (result != GGPO_OK) {
            CoreSetError("RollbackNetplay: Failed to set frame delay");
            ggpo_close_session(ggpoSession);
            ggpoSession = nullptr;
            instancePtr = nullptr;
            return false;
        }

        initialized = true;
        currentFrame = 0;
        
        std::cout << "RollbackNetplay: Initialized successfully!" << std::endl;
        return true;
    }

    void Shutdown()
    {
        if (initialized && ggpoSession != nullptr) {
            ggpo_close_session(ggpoSession);
            ggpoSession = nullptr;
            initialized = false;
            currentFrame = 0;
            
            // Reset the static instance pointer if it's pointing to this instance
            if (instancePtr == this) {
                instancePtr = nullptr;
            }
            
            std::cout << "RollbackNetplay: Session closed" << std::endl;
        }
    }

    bool IsInitialized() const
    {
        return initialized;
    }

    bool AddLocalInput(const uint8_t* input)
    {
        if (!initialized || !ggpoSession) {
            return false;
        }

        // Increment input sequence
        currentInputSequence++;

        // Convert input to the format expected by GGPO
        GGPOErrorCode result = ggpo_add_local_input(
            ggpoSession,
            localPlayerHandle,
            const_cast<void*>(static_cast<const void*>(input)),
            ROLLBACK_INPUT_BYTES);

        return (result == GGPO_OK);
    }

    bool GetSynchronizedInputs(uint8_t* inputs)
    {
        if (!initialized || !ggpoSession) {
            return false;
        }
        
        // Get all player inputs synchronized by GGPO
        GGPOErrorCode result = ggpo_synchronize_input(
            ggpoSession,
            inputs,
            maxPlayers * ROLLBACK_INPUT_BYTES,
            nullptr);

        return (result == GGPO_OK);
    }

    bool AdvanceFrame()
    {
        if (!initialized || !ggpoSession) {
            return false;
        }

        // Process GGPO's internal event queue to handle network events
        GGPOErrorCode result = ggpo_advance_frame(ggpoSession);
        if (result != GGPO_OK) {
            return false;
        }
        
        return true;
    }

    void SetCallbacks(
        bool (*saveState)(void** buffer, int* len, int* checksum, int frame),
        bool (*loadState)(void* buffer, int len),
        void (*freeState)(void* buffer),
        bool (*advanceFrame)())
    {
        saveStateFn = saveState;
        loadStateFn = loadState;
        freeStateFn = freeState;
        advanceFrameFn = advanceFrame;
    }

    static RollbackNetplayImpl* GetInstance() {
        return instancePtr;
    }

    // Get the current input sequence number
    uint32_t GetCurrentInputSequence() const {
        return currentInputSequence;
    }

    // Get the input sequence of the last saved frame (used for state info)
    uint32_t GetLastSavedFrameInputSequence() const {
        return lastSavedFrameInputSequence;
    }

    // Add a method to update metrics based on GGPO events
    void UpdateMetrics(GGPOEvent* event) {
        // Update metrics based on event type
        switch (event->code) {
            case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
                // Network interrupted events could be tracked here
                break;
                
            case GGPO_EVENTCODE_CONNECTION_RESUMED:
                // Network resumed events could be tracked here
                break;
                
            case GGPO_EVENTCODE_CONNECTED_TO_PEER:
                metrics.Reset();
                break;
                
            case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
                // Reset on disconnect
                metrics.Reset();
                break;
                
            case GGPO_EVENTCODE_TIMESYNC:
                // Update frame advantage metrics
                metrics.remoteFrameAdvantage = event->u.timesync.frames_ahead;
                break;
                
            default:
                break;
        }
        
        // Request GGPO network stats to update ping
        GGPONetworkStats stats;
        if (ggpo_get_network_stats(ggpoSession, localPlayerHandle, &stats) == GGPO_OK) {
            metrics.pingMs = stats.network.ping;
            metrics.predictedFrames = stats.timesync.remote_frames_behind;
        }
    }

    // Add a method to get current metrics
    RollbackNetplay::RollbackMetrics GetMetrics() const {
        return metrics;
    }

private:
    // Static instance pointer for callbacks
    static RollbackNetplayImpl* instancePtr;

    // GGPO session
    GGPOSession* ggpoSession;
    
    // Player info
    int localPlayer;
    int maxPlayers;
    bool initialized;
    int currentFrame;
    
    // Player handles
    GGPOPlayerHandle localPlayerHandle;
    GGPOPlayerHandle remotePlayerHandles[GGPO_MAX_PLAYERS - 1];
    
    // Callback functions
    bool (*saveStateFn)(void** buffer, int* len, int* checksum, int frame);
    bool (*loadStateFn)(void* buffer, int len);
    void (*freeStateFn)(void* buffer);
    bool (*advanceFrameFn)();

    // Input tracking for determinism
    uint32_t currentInputSequence;
    uint32_t lastSavedFrameInputSequence;

    // Metrics tracking
    struct RollbackMetrics {
        int rollbackFrames;          // Number of frames rolled back
        int totalRollbacks;          // Total number of rollback events
        int predictedFrames;         // Frames that used predicted input
        int maxRollbackFrames;       // Maximum rollback distance
        float avgRollbackFrames;     // Average rollback distance
        int pingMs;                  // Current ping in milliseconds
        int remoteFrameAdvantage;    // Frame advantage of remote player
        
        // Reset metrics
        void Reset() {
            rollbackFrames = 0;
            totalRollbacks = 0;
            predictedFrames = 0;
            maxRollbackFrames = 0;
            avgRollbackFrames = 0.0f;
            pingMs = 0;
            remoteFrameAdvantage = 0;
        }
    };
    
    // Current metrics
    RollbackNetplay::RollbackMetrics metrics;
};

// Initialize static instance pointer
RollbackNetplayImpl* RollbackNetplayImpl::instancePtr = nullptr;

//
// RollbackNetplay implementation (public interface)
//

RollbackNetplay::RollbackNetplay() : impl(new RollbackNetplayImpl())
{
}

RollbackNetplay::~RollbackNetplay()
{
    Shutdown();
}

bool RollbackNetplay::Initialize(const std::string& address, int port, int player, int maxPlayers, int frameDelay)
{
    return impl->Initialize(address, port, player, maxPlayers, frameDelay);
}

void RollbackNetplay::Shutdown()
{
    impl->Shutdown();
}

bool RollbackNetplay::IsInitialized() const
{
    return impl->IsInitialized();
}

bool RollbackNetplay::AddLocalInput(const uint8_t* input)
{
    return impl->AddLocalInput(input);
}

bool RollbackNetplay::GetSynchronizedInputs(uint8_t* inputs)
{
    return impl->GetSynchronizedInputs(inputs);
}

bool RollbackNetplay::AdvanceFrame()
{
    return impl->AdvanceFrame();
}

void RollbackNetplay::SetCallbacks(
    bool (*saveState)(void** buffer, int* len, int* checksum, int frame),
    bool (*loadState)(void* buffer, int len),
    void (*freeState)(void* buffer),
    bool (*advanceFrame)())
{
    impl->SetCallbacks(saveState, loadState, freeState, advanceFrame);
}

RollbackNetplay::RollbackMetrics RollbackNetplay::GetMetrics() const {
    return impl->GetMetrics();
}

bool RollbackNetplay::RollbackJustOccurred() {
    return impl->RollbackJustOccurred();
} 