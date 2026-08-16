#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <BlockFile.h>

struct LibretroBlockLayout
{
    std::string name;
    size_t payloadSize;
};

// ------------------------------------------------------------
// Counts how many bytes SaveState() would write
// ------------------------------------------------------------
class CBlockFileCounter : public CBlockFile
{
public:
    CBlockFileCounter();

    size_t GetSize() const;
    std::vector<LibretroBlockLayout> GetLayout() const;

    // EXACT signature matches
    void Write(const void* data, uint32_t numBytes) override;
    void Write(bool value) override;
    void Write(const std::string& str) override;
    void NewBlock(const std::string& name, const std::string& comment) override;

private:
    void FinishCurrentBlock();

    size_t m_size;
    size_t m_currentBlockDataStart;
    std::vector<LibretroBlockLayout> m_layout;
};

// ------------------------------------------------------------
// Writes SaveState() directly into a memory buffer
// ------------------------------------------------------------
class CBlockFileMemory : public CBlockFile
{
public:
    CBlockFileMemory(void* data, size_t size);
    CBlockFileMemory(const void* data, size_t size);
    void NewBlock(const std::string& name, const std::string& comment) override;
    Result FindBlock(const std::string &name) override;
    void Write(const void* data, uint32_t numBytes) override;
    void Write(bool value) override;
    void Write(const std::string& str) override;
    unsigned Read(void *data, uint32_t numBytes) override;
    unsigned Read(bool *value) override;
    bool Finish();
    bool HasError() const;
    size_t GetOffset() const;
    bool ValidateLayout(const std::vector<LibretroBlockLayout>& expected) const;
private:
    bool ParseBlock(size_t blockStart,
                    size_t& blockEnd,
                    size_t& dataStart,
                    std::string& blockName) const;

    uint8_t* m_ptr;
    size_t   m_offset;
    size_t   m_capacity;
    size_t   m_currentBlockHeaderPos;
    size_t   m_currentBlockEnd;
    bool     m_writable;
    bool     m_error;
};
