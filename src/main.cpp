#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
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
    std::string sql;
};

struct RecordColumn {
    std::uint64_t serial_type = 0;
    size_t offset = 0;
    size_t size = 0;
};

struct QueryInfo {
    bool valid = false;
    bool is_count = false;
    std::string column_name;
    std::string table_name;
};

struct TableColumnDefinition {
    std::string name;
    bool is_integer_primary_key = false;
};

std::string trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string strip_trailing_semicolon(const std::string& value) {
    std::string result = value;
    while (!result.empty() && result.back() == ';') {
        result.pop_back();
    }
    return result;
}

std::string strip_identifier_quotes(const std::string& value) {
    if (value.size() >= 2) {
        if ((value.front() == '"' && value.back() == '"') ||
            (value.front() == '\'' && value.back() == '\'') ||
            (value.front() == '`' && value.back() == '`') ||
            (value.front() == '[' && value.back() == ']')) {
            return value.substr(1, value.size() - 2);
        }
    }

    return value;
}

std::string to_lower_ascii(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string normalize_identifier(const std::string& value) {
    return to_lower_ascii(strip_identifier_quotes(trim(strip_trailing_semicolon(value))));
}

bool read_big_endian_u16(std::ifstream& file, std::streamoff offset, unsigned int& value) {
    file.clear();
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
    file.clear();
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

bool is_integer_serial_type(std::uint64_t serial_type) {
    return (serial_type >= 1 && serial_type <= 6) || serial_type == 8 || serial_type == 9;
}

std::uint64_t read_unsigned_integer_bytes(
    const std::vector<unsigned char>& data,
    size_t offset,
    size_t size
) {
    std::uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) {
        value = (value << 8) | data[offset + i];
    }
    return value;
}

std::int64_t sign_extend_integer(std::uint64_t value, size_t size) {
    if (size == 0) {
        return 0;
    }

    if (size >= sizeof(std::int64_t)) {
        return static_cast<std::int64_t>(value);
    }

    std::uint64_t sign_bit = 1ULL << ((size * 8) - 1);
    if ((value & sign_bit) != 0) {
        value |= (~0ULL) << (size * 8);
    }

    return static_cast<std::int64_t>(value);
}

bool read_integer_column(
    const std::vector<unsigned char>& page,
    const RecordColumn& column,
    std::int64_t& value
) {
    if (column.serial_type == 8) {
        value = 0;
        return true;
    }

    if (column.serial_type == 9) {
        value = 1;
        return true;
    }

    if (!is_integer_serial_type(column.serial_type)) {
        return false;
    }

    value = sign_extend_integer(
        read_unsigned_integer_bytes(page, column.offset, column.size),
        column.size
    );
    return true;
}

bool read_text_column(
    const std::vector<unsigned char>& page,
    const RecordColumn& column,
    std::string& value,
    bool allow_null = false
) {
    if (column.serial_type == 0 && allow_null) {
        value.clear();
        return true;
    }

    if (!is_text_serial_type(column.serial_type)) {
        return false;
    }

    value.assign(
        reinterpret_cast<const char*>(page.data() + column.offset),
        column.size
    );
    return true;
}

bool read_column_as_string(
    const std::vector<unsigned char>& page,
    const RecordColumn& column,
    std::string& value
) {
    if (column.serial_type == 0) {
        value.clear();
        return true;
    }

    if (is_text_serial_type(column.serial_type)) {
        return read_text_column(page, column, value);
    }

    if (is_integer_serial_type(column.serial_type)) {
        std::int64_t integer_value;
        if (!read_integer_column(page, column, integer_value)) {
            return false;
        }

        value = std::to_string(integer_value);
        return true;
    }

    if (column.serial_type == 7) {
        std::uint64_t bits = read_unsigned_integer_bytes(page, column.offset, column.size);
        double floating_point_value;
        std::memcpy(&floating_point_value, &bits, sizeof(floating_point_value));

        std::ostringstream output;
        output << floating_point_value;
        value = output.str();
        return true;
    }

    return false;
}

bool parse_record_columns(
    const std::vector<unsigned char>& page,
    size_t record_offset,
    size_t payload_size,
    std::vector<RecordColumn>& columns
) {
    if (record_offset > page.size() || payload_size > page.size() - record_offset) {
        return false;
    }

    size_t payload_end = record_offset + payload_size;

    std::uint64_t header_size;
    size_t bytes_read;
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

    columns.clear();
    size_t body_offset = header_end;
    for (std::uint64_t serial_type : serial_types) {
        size_t value_size = serial_type_size(serial_type);
        if (body_offset > payload_end || value_size > payload_end - body_offset) {
            return false;
        }

        columns.push_back({serial_type, body_offset, value_size});
        body_offset += value_size;
    }

    return true;
}

bool read_table_leaf_cell_columns(
    const std::vector<unsigned char>& page,
    size_t cell_offset,
    std::uint64_t& row_id,
    std::vector<RecordColumn>& columns
) {
    std::uint64_t payload_size;
    size_t bytes_read;
    if (!read_varint(page, cell_offset, payload_size, bytes_read)) {
        return false;
    }

    size_t offset = cell_offset + bytes_read;

    if (!read_varint(page, offset, row_id, bytes_read)) {
        return false;
    }

    offset += bytes_read;
    return parse_record_columns(page, offset, static_cast<size_t>(payload_size), columns);
}

bool read_leaf_table_page_cell_offsets(
    const std::vector<unsigned char>& page,
    unsigned int page_number,
    std::vector<unsigned int>& cell_offsets
) {
    size_t header_offset = page_header_offset(page_number);
    if (header_offset + 8 > page.size()) {
        return false;
    }

    if (page[header_offset] != 0x0d) {
        return false;
    }

    unsigned int cell_count = read_big_endian_u16(page, header_offset + 3);
    size_t pointer_array_offset = header_offset + 8;

    cell_offsets.clear();
    for (unsigned int i = 0; i < cell_count; ++i) {
        size_t pointer_offset = pointer_array_offset + (i * 2);
        if (pointer_offset + 1 >= page.size()) {
            return false;
        }

        unsigned int cell_offset = read_big_endian_u16(page, pointer_offset);
        if (cell_offset >= page.size()) {
            return false;
        }

        cell_offsets.push_back(cell_offset);
    }

    return true;
}

bool extract_schema_entry_from_cell(
    const std::vector<unsigned char>& page,
    size_t cell_offset,
    SchemaEntry& entry
) {
    std::vector<RecordColumn> columns;
    std::uint64_t row_id;
    if (!read_table_leaf_cell_columns(page, cell_offset, row_id, columns)) {
        return false;
    }

    if (columns.size() < 5) {
        return false;
    }

    if (!read_text_column(page, columns[0], entry.type) ||
        !read_text_column(page, columns[1], entry.name) ||
        !read_text_column(page, columns[2], entry.table_name) ||
        !read_text_column(page, columns[4], entry.sql, true)) {
        return false;
    }

    std::int64_t root_page;
    if (!read_integer_column(page, columns[3], root_page)) {
        return false;
    }

    entry.root_page = static_cast<std::uint64_t>(root_page);
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

    std::vector<unsigned int> cell_offsets;
    if (!read_leaf_table_page_cell_offsets(page, 1, cell_offsets)) {
        return false;
    }

    schema_entries.clear();
    for (unsigned int cell_offset : cell_offsets) {
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

bool lookup_table_schema(
    std::ifstream& database_file,
    unsigned int page_size,
    const std::string& table_name,
    SchemaEntry& table_schema
) {
    std::vector<SchemaEntry> schema_entries;
    if (!read_schema_entries(database_file, page_size, schema_entries)) {
        return false;
    }

    std::string normalized_table_name = normalize_identifier(table_name);
    for (const SchemaEntry& entry : schema_entries) {
        if (entry.type == "table" &&
            normalize_identifier(entry.table_name) == normalized_table_name) {
            table_schema = entry;
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

std::string read_identifier_token(const std::string& definition) {
    std::string trimmed_definition = trim(definition);
    if (trimmed_definition.empty()) {
        return {};
    }

    char first_character = trimmed_definition.front();
    if (first_character == '"' || first_character == '\'' || first_character == '`') {
        size_t closing_quote = trimmed_definition.find(first_character, 1);
        if (closing_quote == std::string::npos) {
            return trimmed_definition;
        }
        return trimmed_definition.substr(0, closing_quote + 1);
    }

    if (first_character == '[') {
        size_t closing_bracket = trimmed_definition.find(']', 1);
        if (closing_bracket == std::string::npos) {
            return trimmed_definition;
        }
        return trimmed_definition.substr(0, closing_bracket + 1);
    }

    size_t token_end = 0;
    while (token_end < trimmed_definition.size() &&
           !std::isspace(static_cast<unsigned char>(trimmed_definition[token_end]))) {
        ++token_end;
    }

    return trimmed_definition.substr(0, token_end);
}

std::vector<std::string> split_top_level_comma_separated_values(const std::string& value) {
    std::vector<std::string> parts;
    std::string current_part;
    int parenthesis_depth = 0;
    char quote_terminator = '\0';

    for (char character : value) {
        if (quote_terminator != '\0') {
            current_part += character;
            if (character == quote_terminator) {
                quote_terminator = '\0';
            }
            continue;
        }

        if (character == '"' || character == '\'' || character == '`') {
            quote_terminator = character;
            current_part += character;
            continue;
        }

        if (character == '[') {
            quote_terminator = ']';
            current_part += character;
            continue;
        }

        if (character == '(') {
            ++parenthesis_depth;
            current_part += character;
            continue;
        }

        if (character == ')') {
            if (parenthesis_depth > 0) {
                --parenthesis_depth;
            }
            current_part += character;
            continue;
        }

        if (character == ',' && parenthesis_depth == 0) {
            std::string trimmed_part = trim(current_part);
            if (!trimmed_part.empty()) {
                parts.push_back(trimmed_part);
            }
            current_part.clear();
            continue;
        }

        current_part += character;
    }

    std::string trimmed_part = trim(current_part);
    if (!trimmed_part.empty()) {
        parts.push_back(trimmed_part);
    }

    return parts;
}

bool extract_table_columns_from_create_sql(
    const std::string& create_sql,
    std::vector<TableColumnDefinition>& column_definitions
) {
    size_t open_parenthesis = create_sql.find('(');
    size_t close_parenthesis = create_sql.rfind(')');
    if (open_parenthesis == std::string::npos ||
        close_parenthesis == std::string::npos ||
        close_parenthesis <= open_parenthesis) {
        return false;
    }

    std::vector<std::string> definitions = split_top_level_comma_separated_values(
        create_sql.substr(open_parenthesis + 1, close_parenthesis - open_parenthesis - 1)
    );

    column_definitions.clear();
    for (const std::string& definition : definitions) {
        std::string identifier = read_identifier_token(definition);
        if (identifier.empty()) {
            continue;
        }

        std::string normalized_identifier = normalize_identifier(identifier);
        if (normalized_identifier == "constraint" ||
            normalized_identifier == "primary" ||
            normalized_identifier == "foreign" ||
            normalized_identifier == "unique" ||
            normalized_identifier == "check") {
            continue;
        }

        std::string remainder = trim(definition.substr(identifier.size()));
        std::string normalized_remainder = to_lower_ascii(remainder);

        TableColumnDefinition column_definition;
        column_definition.name = strip_identifier_quotes(trim(identifier));

        std::istringstream remainder_stream(remainder);
        std::string type_name;
        remainder_stream >> type_name;
        column_definition.is_integer_primary_key =
            to_lower_ascii(type_name) == "integer" &&
            normalized_remainder.find("primary key") != std::string::npos;

        column_definitions.push_back(column_definition);
    }

    return !column_definitions.empty();
}

int find_column_index(
    const std::vector<TableColumnDefinition>& column_definitions,
    const std::string& target_column_name
) {
    std::string normalized_target = normalize_identifier(target_column_name);
    for (size_t i = 0; i < column_definitions.size(); ++i) {
        if (normalize_identifier(column_definitions[i].name) == normalized_target) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

bool read_table_column_values(
    std::ifstream& database_file,
    unsigned int page_size,
    unsigned int page_number,
    int rowid_column_index,
    size_t column_index,
    std::vector<std::string>& values
) {
    std::vector<unsigned char> page;
    if (!read_page(database_file, page_size, page_number, page)) {
        return false;
    }

    std::vector<unsigned int> cell_offsets;
    if (!read_leaf_table_page_cell_offsets(page, page_number, cell_offsets)) {
        return false;
    }

    values.clear();
    for (unsigned int cell_offset : cell_offsets) {
        std::vector<RecordColumn> columns;
        std::uint64_t row_id;
        if (!read_table_leaf_cell_columns(page, cell_offset, row_id, columns)) {
            return false;
        }

        if (column_index >= columns.size() && static_cast<int>(column_index) != rowid_column_index) {
            return false;
        }

        std::string value;
        if (static_cast<int>(column_index) == rowid_column_index) {
            value = std::to_string(row_id);
        } else {
            if (!read_column_as_string(page, columns[column_index], value)) {
                return false;
            }
        }

        values.push_back(value);
    }

    return true;
}

QueryInfo parse_query(const std::string& query) {
    std::istringstream query_stream(query);
    std::vector<std::string> tokens;
    std::string token;

    while (query_stream >> token) {
        tokens.push_back(token);
    }

    QueryInfo parsed_query;
    if (tokens.size() != 4 ||
        to_lower_ascii(tokens[0]) != "select" ||
        to_lower_ascii(tokens[2]) != "from") {
        return parsed_query;
    }

    parsed_query.valid = true;
    parsed_query.is_count = to_lower_ascii(tokens[1]) == "count(*)";
    parsed_query.column_name = strip_trailing_semicolon(tokens[1]);
    parsed_query.table_name = strip_trailing_semicolon(tokens[3]);
    return parsed_query;
}

int main(int argc, char* argv[]) {
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
        return 0;
    }

    if (command == ".tables") {
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
        return 0;
    }

    QueryInfo query = parse_query(command);
    if (!query.valid) {
        std::cerr << "Failed to parse query" << std::endl;
        return 1;
    }

    SchemaEntry table_schema;
    if (!lookup_table_schema(database_file, page_size, query.table_name, table_schema)) {
        std::cerr << "Failed to find table in sqlite_schema" << std::endl;
        return 1;
    }

    if (query.is_count) {
        unsigned int row_count;
        if (!read_btree_page_cell_count(
                database_file,
                page_size,
                static_cast<unsigned int>(table_schema.root_page),
                row_count
            )) {
            std::cerr << "Failed to read table b-tree page" << std::endl;
            return 1;
        }

        std::cout << row_count << std::endl;
        return 0;
    }

    std::vector<TableColumnDefinition> column_definitions;
    if (!extract_table_columns_from_create_sql(table_schema.sql, column_definitions)) {
        std::cerr << "Failed to parse CREATE TABLE statement" << std::endl;
        return 1;
    }

    int column_index = find_column_index(column_definitions, query.column_name);
    if (column_index < 0) {
        std::cerr << "Failed to find column in CREATE TABLE statement" << std::endl;
        return 1;
    }

    int rowid_column_index = -1;
    for (size_t i = 0; i < column_definitions.size(); ++i) {
        if (column_definitions[i].is_integer_primary_key) {
            rowid_column_index = static_cast<int>(i);
            break;
        }
    }

    std::vector<std::string> values;
    if (!read_table_column_values(
            database_file,
            page_size,
            static_cast<unsigned int>(table_schema.root_page),
            rowid_column_index,
            static_cast<size_t>(column_index),
            values
        )) {
        std::cerr << "Failed to read table rows" << std::endl;
        return 1;
    }

    for (const std::string& value : values) {
        std::cout << value << std::endl;
    }

    return 0;
}
