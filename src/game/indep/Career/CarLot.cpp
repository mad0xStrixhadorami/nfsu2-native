#include "game/indep/Career/CarLot.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "game/bWare/bChunk.h"
#include "game/indep/World/CarMaterial.h"

namespace Career {

bool CarLotTable::Load(const uint8_t *bundle, size_t size)
{
    m_entries.clear();
    m_loaded = false;
    if (!bundle || size < 8)
        return false;
    const bChunk *container = bFindChunk(bundle, bundle + size, 0x80034a10);
    if (!container)
        return false;
    const bChunk *table = bFindChunk(container->Data(), container->Data() + container->size, 0x00034a1f);
    if (!table || table->size % 12 != 0)
        return false;
    for (size_t i = 0; i < table->size / 12; ++i) {
        CarLotEntry e;
        memcpy(&e.carHash, table->Data() + i * 12, 4);
        memcpy(&e.eventA, table->Data() + i * 12 + 4, 4);
        memcpy(&e.eventB, table->Data() + i * 12 + 8, 4);
        m_entries.push_back(e);
    }
    m_loaded = true;
    printf("[Career] car lot: %zu cars gated by events\n", m_entries.size());
    return true;
}

uint32_t CarLotTable::UnlockEvent(const std::string &folder) const
{
    std::string key = folder;
    for (char &c : key)
        c = char(toupper(static_cast<unsigned char>(c)));
    const uint32_t hash = CarMaterial::NameHash(key.c_str());
    for (const CarLotEntry &e : m_entries)
        if (e.carHash == hash)
            return e.eventB;
    return 0;
}

bool CarLotTable::Available(const std::string &folder, const std::vector<uint32_t> &completed) const
{
    const uint32_t ev = UnlockEvent(folder);
    return ev == 0 || std::find(completed.begin(), completed.end(), ev) != completed.end();
}

CarLotTable &CarLotTable::Get()
{
    static CarLotTable table;
    return table;
}

} // namespace Career
