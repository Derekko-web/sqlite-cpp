#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

struct SchemaEntry {
    std::string type;
    std::string name;
    std::string table_name;
    std::uint64_t root_page = 0;
};

bool read_big_endian_u16(std::ifstream& file, std::streamoff offset, unsigned int& value) {
    file.seekg(offset);

    char buffer[2];
    file.read(buffer, sizeof(buffer));
    if (file.gcount() != sizeof(buffer)) {
        return false;
    }

    value =
        (static_cast<unsigned char>(buffer[0]) << 8) |
        static_cast<unsigned char>(buffer[1]);

    return true;
}

unsigned int read_big_endian_u16(const std::vector<unsigned char>& data, size_t offset) {
    return
        (static_cast<unsigned int>(data[offset]) << 8) |
        static_cast<unsigned int>(data[offset + 1]);
}

bool read_page(
    std::ifstream& file,
    unsigned int page_size,
    unsigned int page_number,
    std::vector<unsigned char>& page
) {
    page.resize(page_size);
    file.seekg(static_cast<std::streamoff>(page_number - 1) * page_size);
    file.read(reinterpret_cast<char*>(page.data()), page.size());
    return file.gcount() == static_cast<std::streamsize>(page.size());
}

size_t page_header_offset(unsigned int page_number) {
    return page_number == 1 ? 100 : 0;
}

bool read_varint(
    const std::vector<unsigned char>& data,
    size_t offset,
    std::uint64_t& value,
    size_t& bytes_read
) {
    value = 0;

    for (size_t i = 0; i < 8; ++i) {
        if (offset + i >= data.size()) {
            return false;
        }

        unsigned char byte = data[offset + i];
        if ((byte & 0x80) == 0) {
            value = (value << 7) | byte;
            bytes_read = i + 1;
            return true;
        }

        value = (value << 7) | (byte & 0x7f);
    }

    if (offset + 8 >= data.size()) {
        return false;
    }

    value = (value << 8) | data[offset + 8];
    bytes_read = 9;
    return true;
}

size_t serial_type_size(std::uint64_t serial_type) {
    switch (serial_type) {
        case 0:
        case 8:
        case 9:
        case 10:
        case 11:
            return 0;
        case 1:
            return 1;
        case 2:
            return 2;
        case 3:
            return 3;
        case 4:
            return 4;
        case 5:
            return 6;
        case 6:
        case 7:
            return 8;
        default:
            if (serial_type >= 12) {
                return static_cast<size_t>((serial_type - (serial_type % 2 == 0 ? 12 : 13)) / 2);
            }
            return 0;
    }
}

bool is_text_serial_type(std::uint64_t serial_type) {
    return serial_type >= 13 && serial_type % 2 == 1;
}

std::uint64_t read_integer_value(
    const std::vector<unsigned char>& data,
    size_t offset,
    std::uint64_t serial_type
) {
    switch (serial_type) {
        case 0:
        case 8:
            return 0;
        case 9:
            return 1;
        default: {
            size_t value_size = serial_type_size(serial_type);
            std::uint64_t value = 0;
            for (size_t i = 0; i < value_size; ++i) {
                value = (value << 8) | data[offset + i];
            }
            return value;
        }
    }
}

bool extract_schema_entry_from_cell(
    const std::vector<unsigned char>& page,
    size_t cell_offset,
    SchemaEntry& entry
) {
    std::uint64_t payload_size;
    size_t bytes_read;
    if (!read_varint(page, cell_offset, payload_size, bytes_read)) {
        return false;
    }

    size_t offset = cell_offset + bytes_read;

    std::uint64_t row_id;
    if (!read_varint(page, offset, row_id, bytes_read)) {
        return false;
    }

    offset += bytes_read;
    size_t record_offset = offset;
    size_t payload_end = record_offset + static_cast<size_t>(payload_size);
    if (payload_end > page.size()) {
        return false;
    }

    std::uint64_t header_size;
    if (!read_varint(page, record_offset, header_size, bytes_read)) {
        return false;
    }

    size_t header_end = record_offset + static_cast<size_t>(header_size);
    if (header_end > payload_end) {
        return false;
    }

    size_t serial_type_offset = record_offset + bytes_read;
    std::vector<std::uint64_t> serial_types;
    while (serial_type_offset < header_end) {
        std::uint64_t serial_type;
        if (!read_varint(page, serial_type_offset, serial_type, bytes_read)) {
            return false;
        }

        serial_types.push_back(serial_type);
        serial_type_offset += bytes_read;
    }

    if (serial_type_offset != header_end) {
        return false;
    }

    if (serial_types.size() < 4) {
        return false;
    }

    size_t body_offset = header_end;
    for (size_t i = 0; i < serial_types.size(); ++i) {
        size_t value_size = serial_type_size(serial_types[i]);
        if (body_offset + value_size > payload_end) {
            return false;
        }

        if (i <= 2) {
            if (!is_text_serial_type(serial_types[i])) {
                return false;
            }

            std::string value(
                reinterpret_cast<const char*>(page.data() + body_offset),
                value_size
            );

            if (i == 0) {
                entry.type = value;
            } else if (i == 1) {
                entry.name = value;
            } else {
                entry.table_name = value;
            }
        } else if (i == 3) {
            entry.root_page = read_integer_value(page, body_offset, serial_types[i]);
        }

        body_offset += value_size;
    }

    return true;
}

bool read_schema_entries(
    std::ifstream& database_file,
    unsigned int page_size,
    std::vector<SchemaEntry>& schema_entries
) {
    std::vector<unsigned char> page;
    if (!read_page(database_file, page_size, 1, page)) {
        return false;
    }

    const size_t schema_page_header_offset = page_header_offset(1);
    const size_t cell_count_offset = schema_page_header_offset + 3;
    const size_t cell_pointer_array_offset = schema_page_header_offset + 8;

    unsigned int cell_count = read_big_endian_u16(page, cell_count_offset);
    schema_entries.clear();

    for (unsigned int i = 0; i < cell_count; ++i) {
        size_t pointer_offset = cell_pointer_array_offset + (i * 2);
        if (pointer_offset + 1 >= page.size()) {
            return false;
        }

        unsigned int cell_offset = read_big_endian_u16(page, pointer_offset);
        if (cell_offset >= page.size()) {
            return false;
        }

        SchemaEntry entry;
        if (!extract_schema_entry_from_cell(page, cell_offset, entry)) {
            return false;
        }

        schema_entries.push_back(entry);
    }

    return true;
}

bool read_table_names(
    std::ifstream& database_file,
    unsigned int page_size,
    std::vector<std::string>& table_names
) {
    std::vector<SchemaEntry> schema_entries;
    if (!read_schema_entries(database_file, page_size, schema_entries)) {
        return false;
    }

    table_names.clear();
    for (const SchemaEntry& entry : schema_entries) {
        if (entry.type == "table" && !entry.table_name.starts_with("sqlite_")) {
            table_names.push_back(entry.table_name);
        }
    }

    std::sort(table_names.begin(), table_names.end());
    return true;
}

bool lookup_table_root_page(
    std::ifstream& database_file,
    unsigned int page_size,
    const std::string& table_name,
    unsigned int& root_page
) {
    std::vector<SchemaEntry> schema_entries;
    if (!read_schema_entries(database_file, page_size, schema_entries)) {
        return false;
    }

    for (const SchemaEntry& entry : schema_entries) {
        if (entry.type == "table" && entry.table_name == table_name) {
            root_page = static_cast<unsigned int>(entry.root_page);
            return true;
        }
    }

    return false;
}

bool read_btree_page_cell_count(
    std::ifstream& database_file,
    unsigned int page_size,
    unsigned int page_number,
    unsigned int& cell_count
) {
    std::vector<unsigned char> page;
    if (!read_page(database_file, page_size, page_number, page)) {
        return false;
    }

    size_t header_offset = page_header_offset(page_number);
    if (header_offset + 4 >= page.size()) {
        return false;
    }

    cell_count = read_big_endian_u16(page, header_offset + 3);
    return true;
}

std::string parse_table_name_from_count_query(const std::string& query) {
    std::istringstream query_stream(query);
    std::string token;
    std::string table_name;

    while (query_stream >> token) {
        table_name = token;
    }

    while (!table_name.empty() && table_name.back() == ';') {
        table_name.pop_back();
    }

    return table_name;
}

int main(int argc, char* argv[]) {
    // Flush after every std::cout / std::cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    if (argc != 3) {
        std::cerr << "Expected two arguments" << std::endl;
        return 1;
    }

    std::string database_file_path = argv[1];
    std::string command = argv[2];

    std::ifstream database_file(database_file_path, std::ios::binary);
    if (!database_file) {
        std::cerr << "Failed to open the database file" << std::endl;
        return 1;
    }

    unsigned int page_size;
    if (!read_big_endian_u16(database_file, 16, page_size)) {
        std::cerr << "Failed to read the database header" << std::endl;
        return 1;
    }

    if (page_size == 1) {
        page_size = 65536;
    }

    if (command == ".dbinfo") {
        unsigned int number_of_tables;
        if (!read_big_endian_u16(database_file, 100 + 3, number_of_tables)) {
            std::cerr << "Failed to read the sqlite_schema page header" << std::endl;
            return 1;
        }

        std::cout << "database page size: " << page_size << std::endl;
        std::cout << "number of tables: " << number_of_tables << std::endl;
    } else if (command == ".tables") {
        std::vector<std::string> table_names;
        if (!read_table_names(database_file, page_size, table_names)) {
            std::cerr << "Failed to read sqlite_schema" << std::endl;
            return 1;
        }

        for (size_t i = 0; i < table_names.size(); ++i) {
            if (i > 0) {
                std::cout << " ";
            }
            std::cout << table_names[i];
        }
        std::cout << std::endl;
    } else {
        std::string table_name = parse_table_name_from_count_query(command);
        if (table_name.empty()) {
            std::cerr << "Failed to parse table name from query" << std::endl;
            return 1;
        }

        unsigned int root_page;
        if (!lookup_table_root_page(database_file, page_size, table_name, root_page)) {
            std::cerr << "Failed to find table in sqlite_schema" << std::endl;
            return 1;
        }

        unsigned int row_count;
        if (!read_btree_page_cell_count(database_file, page_size, root_page, row_count)) {
            std::cerr << "Failed to read table b-tree page" << std::endl;
            return 1;
        }

        std::cout << row_count << std::endl;
    }

    return 0;
}
