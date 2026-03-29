#include <fstream>
#include <iostream>

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

    if (command == ".dbinfo") {
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

        unsigned int number_of_tables;
        if (!read_big_endian_u16(database_file, 100 + 3, number_of_tables)) {
            std::cerr << "Failed to read the sqlite_schema page header" << std::endl;
            return 1;
        }

        std::cout << "database page size: " << page_size << std::endl;
        std::cout << "number of tables: " << number_of_tables << std::endl;
    }

    return 0;
}
