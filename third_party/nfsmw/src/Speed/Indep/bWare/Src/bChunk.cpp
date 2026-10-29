#include "Speed/Indep/bWare/Inc/bChunk.hpp"
#include "Speed/Indep/bWare/Inc/bWare.hpp"

int bChunkLoaderFunctionNull(bChunk *chunk) {
    return static_cast<int>(chunk->GetID() == 0);
}

bChunkLoader bChunkLoaderNull(0, bChunkLoaderFunctionNull, bChunkLoaderFunctionNull);

// the loader registry (the upstream sources never defined it: no chunk loader was ever constructed)
bChunkLoader *bChunkLoader::sLoaderTable[64] = {};
unsigned char bChunkLoader::sNumLoaders[64] = {};

bChunkLoader::bChunkLoader(unsigned int id, int (*loader)(bChunk *), int (*unloader)(bChunk *)) {
    this->LoaderFunction = loader;
    this->UnloaderFunction = unloader;

    int hash = bChunkLoader::GetHash(id);
    this->ID = id;
    this->Next = bChunkLoader::sLoaderTable[hash];
    bChunkLoader::sLoaderTable[hash] = this;
    bChunkLoader::sNumLoaders[hash]++;
}

bChunkLoader *bChunkLoader::FindLoader(unsigned int id) {
    int hash = bChunkLoader::GetHash(id);
    bChunkLoader *loader = bChunkLoader::sLoaderTable[hash];
    while (loader != nullptr) {
        if (loader->ID == id) {
            return loader;
        }
        loader = loader->Next;
    }
    return nullptr;
}

// STRIPPED
unsigned int bChunkLoader::CallLoaders(bChunk *chunks, int sizeof_chunks, bool abort_on_error) {
    bChunk *last_chunk = GetLastChunk(chunks, sizeof_chunks);
    for (bChunk *chunk = chunks; chunk != last_chunk; chunk = chunk->GetNext()) {
        bChunkLoader *loader = FindLoader(chunk->GetID());
        if (loader != nullptr) {
            if (loader->GetLoaderFunction()(chunk)) {
                continue;
            }
        }
        if (abort_on_error) {
            return chunk->GetID();
        }
    }

    return 0;
}

// Mirrors CallLoaders with each loader's unloader function; returns the id of the first chunk that failed (0 = all done).
unsigned int bChunkLoader::CallUnloaders(bChunk *chunks, int sizeof_chunks, bool abort_on_error) {
    bChunk *last_chunk = GetLastChunk(chunks, sizeof_chunks);
    for (bChunk *chunk = chunks; chunk != last_chunk; chunk = chunk->GetNext()) {
        bChunkLoader *loader = FindLoader(chunk->GetID());
        if (loader != nullptr && loader->GetUnloaderFunction() != nullptr) {
            if (loader->GetUnloaderFunction()(chunk)) {
                continue;
            }
        }
        if (abort_on_error) {
            return chunk->GetID();
        }
    }

    return 0;
}

void bChunkCarpHeader::PlatformEndianSwap() {
    bPlatEndianSwap(&this->mCrpSize);
    bPlatEndianSwap(&this->mSectionNumber);
    bPlatEndianSwap(&this->mFlags);
}

// STRIPPED
bool bValidateChunksRecursive(bChunk *the_chunk, int32 expected_level_size, int32 *current_offset, long max_size, bChunk *min_pointer,
                              bChunk *max_pointer) {}

// True when the chunk sizes tile exactly the `sizeof_chunks` bytes (nested chunks are walked too).
bool bValidateChunks(bChunk *first_chunk, int sizeof_chunks) {
    int offset = 0;
    while (offset < sizeof_chunks) {
        if (sizeof_chunks - offset < static_cast<int>(sizeof(bChunk)))
            return false;
        bChunk *chunk = reinterpret_cast<bChunk *>(reinterpret_cast<char *>(first_chunk) + offset);
        const int size = chunk->GetSize();
        if (size < 0 || offset + static_cast<int>(sizeof(bChunk)) + size > sizeof_chunks)
            return false;
        if (chunk->IsNestedChunk() && !bValidateChunks(reinterpret_cast<bChunk *>(chunk->GetData()), size))
            return false;
        offset += static_cast<int>(sizeof(bChunk)) + size;
    }
    return offset == sizeof_chunks;
}
