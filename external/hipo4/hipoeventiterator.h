#pragma once

#include "bank.h"
#include "dictionary.h"
#include "event.h"
#include "reader.h"
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hipo {
class hipoeventfile {
    friend class iter_event;

   public:
    class iterator;
    using const_iterator = iterator;

   private:
    std::unique_ptr<hipo::reader> reader;
    dictionary dict;
    mutable std::unordered_map<std::string, bank> bank_templates;

   public:
    explicit hipoeventfile(const std::string& filename) {
        reader = std::make_unique<hipo::reader>();
        reader->open(filename.c_str());
        reader->readDictionary(dict);

        // Pre-create bank templates for all known schemas
        const auto& schema_list = dict.getSchemaList();
        bank_templates.reserve(schema_list.size());
        for (const std::string& name : schema_list) {
            schema& sch = dict.getSchema(name.c_str());
            bank_templates.emplace(name, bank(sch));
        }
    }

    hipoeventfile(hipoeventfile&& other) noexcept = default;
    hipoeventfile& operator=(hipoeventfile&& other) noexcept = default;
    hipoeventfile(const hipoeventfile&) = delete;
    hipoeventfile& operator=(const hipoeventfile&) = delete;

    iterator begin();
    iterator end();
};

class iter_event {
    friend class hipoeventfile;
    friend class hipoeventfile::iterator;

   private:
    event* event_ptr;
    hipoeventfile* file_ptr;
    iter_event(event* ev, hipoeventfile* file) : event_ptr(ev), file_ptr(file) {}

   public:
    iter_event() : event_ptr(nullptr), file_ptr(nullptr) {}
    iter_event(iter_event&&) noexcept = default;
    iter_event& operator=(iter_event&&) noexcept = default;

    inline bank& get_bank(std::string_view bankName) const {
        if (!event_ptr || !file_ptr) throw std::runtime_error("Invalid HipoEvent (no event data)");
        auto it = file_ptr->bank_templates.find(std::string(bankName));
        if (it == file_ptr->bank_templates.end()) throw std::runtime_error("Schema not found for bank: " + std::string(bankName));
        event_ptr->read(it->second);
        return it->second;
    }
};

class hipoeventfile::iterator {
    friend class hipoeventfile;

   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = iter_event;
    using reference = iter_event&;
    using pointer = iter_event*;

   private:
    hipo::reader* reader_ptr;
    hipoeventfile* file_ptr;
    event current_event;
    iter_event current_wrap;
    bool at_end;

    iterator(hipo::reader* rdr, hipoeventfile* file) : reader_ptr(rdr), file_ptr(file), at_end(false) {
        if (!reader_ptr || !reader_ptr->next(current_event)) {
            reader_ptr = nullptr;
            file_ptr = nullptr;
            at_end = true;
        } else {
            current_wrap = iter_event(&current_event, file_ptr);
        }
    }

   public:
    iterator() : reader_ptr(nullptr), file_ptr(nullptr), at_end(true) {}

    inline iterator& operator++() {
        if (!reader_ptr || at_end) return *this;
        if (!reader_ptr->next(current_event)) {
            reader_ptr = nullptr;
            file_ptr = nullptr;
            at_end = true;
        } else {
            current_wrap.event_ptr = &current_event;
            current_wrap.file_ptr = file_ptr;
        }
        return *this;
    }

    inline reference operator*() { return current_wrap; }

    inline pointer operator->() { return &current_wrap; }

    inline bool operator!=(const iterator& other) const { return at_end != other.at_end; }

    inline bool operator==(const iterator& other) const { return at_end == other.at_end; }
};

inline hipoeventfile::iterator hipoeventfile::begin() {
    reader->rewind();
    return {reader.get(), this};
}

inline hipoeventfile::iterator hipoeventfile::end() {
    return {};
}

}  // namespace hipo
