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
#ifdef _WIN32
#define _CRT_RAND_S
#include <cstdlib>
#endif // _WIN32
#include "Netplay.hpp"
#include "RollbackNetplay.hpp"
#include "Library.hpp"
#include "Error.hpp"
#include "Emulation.hpp"
#include "Callback.hpp"
#include "Settings.hpp"

#include "m64p/Api.hpp"
#include <zlib.h>
#include <cstring>
#include <vector>
#include <cstdint>
#include <chrono>

// Core state query parameters
#define M64CORE_RANDOM_SEED 13  // Used to query/set the current RNG seed

// Forward declarations
static bool SaveEmulatorState(void** buffer, int* len, int* checksum, int frame);
static bool LoadEmulatorState(void* buffer, int len);

//
// Local Variables
//

static bool l_HasInitNetplay = false;
static bool l_HasInitRollbackNetplay = false;
static RollbackNetplay* l_RollbackNetplay = nullptr;

// Track current player number and total players for rollback
static int l_RollbackLocalPlayer = 0;
static int l_RollbackMaxPlayers = 0;

// External variable declared in Emulation.cpp
extern uint32_t g_currentInputSequence;

// State performance metrics
struct StateMetrics {
    int64_t totalSaveTimeNs;
    int64_t totalLoadTimeNs;
    uint32_t saveCount;
    uint32_t loadCount;
    size_t totalUncompressedSize;
    size_t totalCompressedSize;
    
    void reset() {
        totalSaveTimeNs = 0;
        totalLoadTimeNs = 0;
        saveCount = 0;
        loadCount = 0;
        totalUncompressedSize = 0;
        totalCompressedSize = 0;
    }
    
    void logMetrics() {
        if (saveCount > 0) {
            double avgSaveTimeMs = (double)totalSaveTimeNs / (saveCount * 1000000.0);
            double compressionRatio = totalUncompressedSize > 0 ? 
                (double)totalCompressedSize / totalUncompressedSize : 1.0;
            
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                "State Save Metrics: Avg time=%.2fms, Saves=%u, Compression=%.2f:1",
                avgSaveTimeMs, saveCount, 1.0 / compressionRatio);
            CoreAddCallbackMessage(CoreDebugMessageType::Info, buffer);
        }
        
        if (loadCount > 0) {
            double avgLoadTimeMs = (double)totalLoadTimeNs / (loadCount * 1000000.0);
            
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                "State Load Metrics: Avg time=%.2fms, Loads=%u",
                avgLoadTimeMs, loadCount);
            CoreAddCallbackMessage(CoreDebugMessageType::Info, buffer);
        }
    }
};

static StateMetrics g_stateMetrics;

// Timer class for measuring performance
class ScopedTimer {
public:
    ScopedTimer(int64_t& totalTime) : m_totalTime(totalTime) {
        m_start = std::chrono::high_resolution_clock::now();
    }
    
    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        m_totalTime += std::chrono::duration_cast<std::chrono::nanoseconds>(end - m_start).count();
    }
    
private:
    int64_t& m_totalTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> m_start;
};

//
// State serialization for rollback netplay
//

// Header for serialized state
struct RollbackStateHeader {
    uint32_t magic;          // Magic number to identify our state format
    uint32_t version;        // State format version
    uint32_t frame;          // Frame number
    uint32_t uncompressedSize; // Size before compression
    uint32_t compressedSize;   // Size after compression
    uint32_t randState;      // RNG state for determinism
    uint32_t inputSequence;  // Input sequence number
    uint32_t reserved[2];    // Reserved for future use
};

#define ROLLBACK_STATE_MAGIC 0x52424B53 // "RBKS"
#define ROLLBACK_STATE_VERSION 1

// Calculate CRC32 checksum
static uint32_t CalculateChecksum(const void* data, size_t size) {
    return crc32(0L, static_cast<const Bytef*>(data), static_cast<uInt>(size));
}

// Compress data using zlib
static bool CompressData(const void* src, int srcLen, void* dst, int* dstLen) {
    // Optimize zlib compression level for speed vs. size
    // Level 1-3: Fast compression
    // Level 4-6: Balanced
    // Level 7-9: Better compression but slower
    const int COMPRESSION_LEVEL = 1; // Fastest compression for real-time performance
    
    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    
    int ret = deflateInit(&strm, COMPRESSION_LEVEL);
    if (ret != Z_OK) {
        return false;
    }
    
    strm.avail_in = srcLen;
    strm.next_in = (Bytef*)src;
    strm.avail_out = *dstLen;
    strm.next_out = (Bytef*)dst;
    
    ret = deflate(&strm, Z_FINISH);
    deflateEnd(&strm);
    
    if (ret != Z_STREAM_END) {
        return false;
    }
    
    *dstLen = strm.total_out;
    return true;
}

// Decompress data using zlib
static bool DecompressData(const void* src, int srcLen, void* dst, int dstLen) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    
    // Initialize zlib
    if (inflateInit(&strm) != Z_OK) {
        return false;
    }
    
    // Set input and output
    strm.avail_in = srcLen;
    strm.next_in = static_cast<Bytef*>(const_cast<void*>(src));
    strm.avail_out = dstLen;
    strm.next_out = static_cast<Bytef*>(dst);
    
    // Decompress
    int result = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    
    if (result != Z_STREAM_END) {
        return false;
    }
    
    return true;
}

// Get the current RNG state from the emulator
static uint32_t GetEmulatorRngState() {
    // This is a placeholder - we need to access the actual RNG state from mupen64plus
    // Ideally we would add an API to the emulator to access this
    uint32_t rngState = 0;
    
    // Try to get the RNG state if the API supports it
    m64p_error ret = m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY, M64CORE_RANDOM_SEED, &rngState);
    if (ret != M64ERR_SUCCESS) {
        // If the API doesn't support it, we'll use a fallback
        // This is less accurate but better than nothing
        static uint32_t fallbackRng = 0;
        fallbackRng++;
        rngState = fallbackRng;
    }
    
    return rngState;
}

// State buffer pool to reduce memory allocations
class StateBufferPool {
public:
    StateBufferPool(size_t bufferSize = 8 * 1024 * 1024, size_t maxBuffers = 4)
        : m_bufferSize(bufferSize), m_maxBuffers(maxBuffers)
    {
        // Pre-allocate at least one buffer
        m_freeBuffers.push_back(static_cast<uint8_t*>(malloc(m_bufferSize)));
    }
    
    ~StateBufferPool() {
        // Free all buffers in both pools
        for (auto buffer : m_freeBuffers) {
            free(buffer);
        }
        for (auto buffer : m_usedBuffers) {
            free(buffer);
        }
    }
    
    // Get a buffer for use
    uint8_t* getBuffer() {
        if (!m_freeBuffers.empty()) {
            // Move a buffer from free list to used list
            uint8_t* buffer = m_freeBuffers.back();
            m_freeBuffers.pop_back();
            m_usedBuffers.push_back(buffer);
            return buffer;
        } else if (m_usedBuffers.size() < m_maxBuffers) {
            // Allocate a new buffer if we're under our maximum
            uint8_t* buffer = static_cast<uint8_t*>(malloc(m_bufferSize));
            if (buffer) {
                m_usedBuffers.push_back(buffer);
                return buffer;
            }
        }
        
        // No buffers available or allocation failed
        return nullptr;
    }
    
    // Release a buffer back to the pool
    void releaseBuffer(void* buffer) {
        if (!buffer) return;
        
        // Find the buffer in the used list
        auto it = std::find(m_usedBuffers.begin(), m_usedBuffers.end(), buffer);
        if (it != m_usedBuffers.end()) {
            // Move it back to the free list
            m_freeBuffers.push_back(*it);
            m_usedBuffers.erase(it);
        }
    }
    
    // Get buffer size
    size_t getBufferSize() const {
        return m_bufferSize;
    }
    
private:
    std::vector<uint8_t*> m_freeBuffers;
    std::vector<uint8_t*> m_usedBuffers;
    size_t m_bufferSize;
    size_t m_maxBuffers;
};

// Global state buffer pool
static StateBufferPool g_stateBufferPool;

// Update the free function to use the buffer pool
static void FreeEmulatorState(void* buffer) {
    g_stateBufferPool.releaseBuffer(buffer);
}

static bool AdvanceEmulatorFrame()
{
    // This is called by GGPO when it's time to advance the emulator by one frame
    return true; // The actual advance happens in the emulation loop
}

//
// Exported Functions
//

CORE_EXPORT bool CoreInitNetplay(std::string address, int port, int player)
{
#ifdef NETPLAY
    std::string error;
    m64p_error ret;
    uint32_t id = 0;

    // initialize random ID
    while (id == 0)
    {
#ifdef _WIN32
        rand_s(&id);
#else
        id = rand();
#endif
        id &= ~0x7;
        id |= player;
    }

    uint32_t version;
    ret = m64p::Core.DoCommand(M64CMD_NETPLAY_GET_VERSION, 0x010001, &version);
    if (ret != M64ERR_SUCCESS)
    { 
        error = "CoreInitNetplay m64p::Core.DoCommand(M64CMD_NETPLAY_GET_VERSION) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_NETPLAY_INIT, port, (void*)address.c_str());
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreInitNetplay m64p::Core.DoCommand(M64CMD_NETPLAY_INIT) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_NETPLAY_CONTROL_PLAYER, player, &id);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreInitNetplay m64p::Core.DoCommand(M64CMD_NETPLAY_CONTROL_PLAYER) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        CoreShutdownNetplay();
        return false;
    }

    l_HasInitNetplay = true;
    return true;
#else
    return false;
#endif // NETPLAY
}

CORE_EXPORT bool CoreHasInitNetplay(void)
{
#ifdef NETPLAY
    return l_HasInitNetplay;
#else
    return false;
#endif // NETPLAY
}

CORE_EXPORT bool CoreShutdownNetplay(void)
{
#ifdef NETPLAY
    std::string error;
    m64p_error ret;

    ret = m64p::Core.DoCommand(M64CMD_NETPLAY_CLOSE, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreShutdownNetplay m64p::Core.DoCommand(M64CMD_NETPLAY_CLOSE) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

    l_HasInitNetplay = false;
    return true;
#else
    return false;
#endif // NETPLAY
}

CORE_EXPORT bool CoreInitRollbackNetplay(std::string address, int port, int player, int maxPlayers)
{
    // Don't allow rollback if normal netplay is already active
    if (l_HasInitNetplay) {
        CoreSetError("Cannot initialize rollback netplay when standard netplay is active");
        return false;
    }

    // Don't allow reinitializing if already active
    if (l_HasInitRollbackNetplay) {
        return true;
    }

    // Make sure player and maxPlayers are valid
    if (player < 1 || player > 4 || maxPlayers < 2 || maxPlayers > 4) {
        CoreSetError("Invalid player number or max players for rollback netplay");
        return false;
    }

    // Store player info
    l_RollbackLocalPlayer = player;
    l_RollbackMaxPlayers = maxPlayers;

    // Create rollback netplay instance if it doesn't exist
    if (!l_RollbackNetplay) {
        l_RollbackNetplay = new RollbackNetplay();
    }

    // Set callbacks for state management
    l_RollbackNetplay->SetCallbacks(
        SaveEmulatorState,
        LoadEmulatorState,
        FreeEmulatorState,
        AdvanceEmulatorFrame);

    // Initialize rollback
    int frameDelay = CoreSettingsGetIntValue(SettingsID::Netplay_RollbackFrameDelay);
    if (!l_RollbackNetplay->Initialize(address, port, player, maxPlayers, frameDelay)) {
        CoreSetError("Failed to initialize rollback netplay");
        delete l_RollbackNetplay;
        l_RollbackNetplay = nullptr;
        return false;
    }

    l_HasInitRollbackNetplay = true;
    return true;
}

CORE_EXPORT bool CoreHasInitRollbackNetplay(void)
{
    return l_HasInitRollbackNetplay;
}

CORE_EXPORT bool CoreShutdownRollbackNetplay(void)
{
    if (!l_HasInitRollbackNetplay || !l_RollbackNetplay) {
        return true; // Nothing to do
    }

    l_RollbackNetplay->Shutdown();
    delete l_RollbackNetplay;
    l_RollbackNetplay = nullptr;
    l_HasInitRollbackNetplay = false;
    
    return true;
}

CORE_EXPORT bool CoreRollbackNetplayAdvanceFrame(void)
{
    if (!l_HasInitRollbackNetplay || !l_RollbackNetplay) {
        return false;
    }

    return l_RollbackNetplay->AdvanceFrame();
}

CORE_EXPORT bool CoreRollbackNetplayAddLocalInput(const uint8_t* input)
{
    if (!l_HasInitRollbackNetplay || !l_RollbackNetplay) {
        return false;
    }

    return l_RollbackNetplay->AddLocalInput(input);
}

CORE_EXPORT bool CoreRollbackNetplayGetSynchronizedInputs(uint8_t* inputs)
{
    if (!l_HasInitRollbackNetplay || !l_RollbackNetplay) {
        return false;
    }

    return l_RollbackNetplay->GetSynchronizedInputs(inputs);
}

CORE_EXPORT bool CoreRollbackNetplayApplyInputs(void)
{
    if (!l_HasInitRollbackNetplay || !l_RollbackNetplay) {
        return false;
    }

    // Get synchronized inputs from GGPO
    uint8_t inputs[ROLLBACK_INPUT_BYTES * 4]; // Max 4 players
    if (!l_RollbackNetplay->GetSynchronizedInputs(inputs)) {
        return false;
    }

    // Apply inputs to virtual controllers for each player
    for (int i = 0; i < l_RollbackMaxPlayers; i++) {
        // Extract the controller input data for this player
        uint8_t* playerInput = &inputs[i * ROLLBACK_INPUT_BYTES];
        
        // Apply to the emulator's controller
        // This function is defined in Emulation.cpp
        extern void ApplyControllerInputs(uint8_t* inputData, int playerIndex);
        ApplyControllerInputs(playerInput, i);
    }

    return true;
}

// Add a function to get rollback stats for visualization
CORE_EXPORT bool CoreRollbackNetplayGetMetrics(int* rollbackFrames, int* totalRollbacks, int* predictedFrames, 
                                               int* maxRollbackFrames, float* avgRollbackFrames, 
                                               int* pingMs, int* remoteFrameAdvantage)
{
    if (!l_HasInitRollbackNetplay || !l_RollbackNetplay) {
        return false;
    }
    
    // Get current metrics
    auto metrics = l_RollbackNetplay->GetMetrics();
    
    // Update out parameters
    if (rollbackFrames) *rollbackFrames = metrics.rollbackFrames;
    if (totalRollbacks) *totalRollbacks = metrics.totalRollbacks;
    if (predictedFrames) *predictedFrames = metrics.predictedFrames;
    if (maxRollbackFrames) *maxRollbackFrames = metrics.maxRollbackFrames;
    if (avgRollbackFrames) *avgRollbackFrames = metrics.avgRollbackFrames;
    if (pingMs) *pingMs = metrics.pingMs;
    if (remoteFrameAdvantage) *remoteFrameAdvantage = metrics.remoteFrameAdvantage;
    
    return true;
}

// Add a simpler function to just get if rollbacks have occurred
CORE_EXPORT bool CoreRollbackNetplayHasRollbacks(void)
{
    if (!l_HasInitRollbackNetplay || !l_RollbackNetplay) {
        return false;
    }
    
    auto metrics = l_RollbackNetplay->GetMetrics();
    return (metrics.totalRollbacks > 0);
}

// Add a function to check if rollback just occurred (for visual indication)
CORE_EXPORT bool CoreRollbackNetplayJustOccurred(void)
{
    if (!l_HasInitRollbackNetplay || !l_RollbackNetplay) {
        return false;
    }
    
    return l_RollbackNetplay->RollbackJustOccurred();
}

// Return the local player index for rollback netplay
CORE_EXPORT int CoreGetNetplayPlayerIndex(void)
{
#ifdef NETPLAY
    // Return 1-based index if rollback is initialized, otherwise 0
    return l_RollbackLocalPlayer > 0 ? l_RollbackLocalPlayer - 1 : 0;
#else
    return 0; // Default to player 1 if netplay is not compiled in
#endif // NETPLAY
}

static bool SaveEmulatorState(void** buffer, int* len, int* checksum, int frame) {
    // Track performance metrics
    ScopedTimer timer(g_stateMetrics.totalSaveTimeNs);
    g_stateMetrics.saveCount++;
    
    // Log metrics every 100 saves
    if (g_stateMetrics.saveCount % 100 == 0) {
        g_stateMetrics.logMetrics();
        g_stateMetrics.reset();
    }
    
    // Use buffer pool instead of allocating directly
    uint8_t* stateBuffer = g_stateBufferPool.getBuffer();
    if (!stateBuffer) {
        CoreSetError("Failed to obtain state buffer from pool");
        return false;
    }
    
    // Use the buffer directly for state saving
    const size_t maxUncompressedSize = g_stateBufferPool.getBufferSize() - sizeof(RollbackStateHeader) - 1024; // Leave space for header and compression overhead
    
    // Save the emulator state to our buffer
    m64p_error ret = m64p::Core.DoCommand(M64CMD_STATE_SAVE, maxUncompressedSize, stateBuffer + sizeof(RollbackStateHeader) + 1024);
    if (ret != M64ERR_SUCCESS) {
        g_stateBufferPool.releaseBuffer(stateBuffer);
        CoreSetError("Failed to save emulator state");
        return false;
    }
    
    // Temporary buffer points to the start of the actual state data
    uint8_t* tempBuffer = stateBuffer + sizeof(RollbackStateHeader) + 1024;
    
    // Find out how big the state actually is
    size_t actualUncompressedSize = maxUncompressedSize;
    for (size_t i = maxUncompressedSize; i > 0; i--) {
        if (tempBuffer[i-1] != 0) {
            actualUncompressedSize = i;
            break;
        }
    }
    
    // Track uncompressed size for metrics
    g_stateMetrics.totalUncompressedSize += actualUncompressedSize;
    
    // Compress the state in-place (use the free space we left at the beginning)
    uint8_t* compressedDataStart = stateBuffer + sizeof(RollbackStateHeader);
    int compressedSize = 1024; // Size of the buffer we reserved for compressed data
    
    if (!CompressData(tempBuffer, actualUncompressedSize, compressedDataStart, &compressedSize)) {
        g_stateBufferPool.releaseBuffer(stateBuffer);
        CoreSetError("Failed to compress state data");
        return false;
    }
    
    // Track compressed size for metrics
    g_stateMetrics.totalCompressedSize += compressedSize;
    
    // Set up the header
    RollbackStateHeader* header = reinterpret_cast<RollbackStateHeader*>(stateBuffer);
    header->magic = ROLLBACK_STATE_MAGIC;
    header->version = ROLLBACK_STATE_VERSION;
    header->frame = frame;
    header->uncompressedSize = actualUncompressedSize;
    header->compressedSize = compressedSize;
    header->randState = GetEmulatorRngState();
    header->inputSequence = g_currentInputSequence;
    header->reserved[0] = 0;
    header->reserved[1] = 0;
    
    // Calculate checksum of the uncompressed data
    *checksum = CalculateChecksum(tempBuffer, actualUncompressedSize);
    
    // Set the output parameters
    *buffer = stateBuffer;
    *len = sizeof(RollbackStateHeader) + compressedSize;
    
    return true;
}

// Improved function to load emulator state
static bool LoadEmulatorState(void* buffer, int len) {
    // Track performance metrics
    ScopedTimer timer(g_stateMetrics.totalLoadTimeNs);
    g_stateMetrics.loadCount++;
    
    if (!buffer || len <= sizeof(RollbackStateHeader)) {
        CoreSetError("Invalid state buffer or size");
        return false;
    }
    
    // Extract the header
    RollbackStateHeader* header = static_cast<RollbackStateHeader*>(buffer);
    
    // Verify the magic number and version
    if (header->magic != ROLLBACK_STATE_MAGIC) {
        CoreSetError("Invalid state format (wrong magic number)");
        return false;
    }
    
    if (header->version != ROLLBACK_STATE_VERSION) {
        CoreSetError("Unsupported state version");
        return false;
    }
    
    // Verify the compressed size matches what we expect
    if (header->compressedSize + sizeof(RollbackStateHeader) > len) {
        CoreSetError("State buffer is smaller than expected");
        return false;
    }
    
    // Get a buffer from the pool for uncompressed data
    uint8_t* uncompressedBuffer = g_stateBufferPool.getBuffer();
    if (!uncompressedBuffer) {
        CoreSetError("Failed to obtain buffer from pool for decompression");
        return false;
    }
    
    // Decompress the state
    int uncompressedSize = header->uncompressedSize;
    if (!DecompressData(
            static_cast<uint8_t*>(buffer) + sizeof(RollbackStateHeader), 
            header->compressedSize,
            uncompressedBuffer,
            uncompressedSize)) {
        g_stateBufferPool.releaseBuffer(uncompressedBuffer);
        CoreSetError("Failed to decompress state data");
        return false;
    }
    
    // Load the state into the emulator
    m64p_error ret = m64p::Core.DoCommand(M64CMD_STATE_LOAD, header->uncompressedSize, uncompressedBuffer);
    if (ret != M64ERR_SUCCESS) {
        g_stateBufferPool.releaseBuffer(uncompressedBuffer);
        CoreSetError("Failed to load emulator state");
        return false;
    }
    
    // Update the global input sequence to match the loaded state
    g_currentInputSequence = header->inputSequence;
    
    // Return the buffer to the pool
    g_stateBufferPool.releaseBuffer(uncompressedBuffer);
    
    return true;
}