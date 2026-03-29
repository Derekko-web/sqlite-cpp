#include <fstream>
#include <iostream>

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

        database_file.seekg(16);

        char buffer[2];
        database_file.read(buffer, sizeof(buffer));
        if (database_file.gcount() != sizeof(buffer)) {
            std::cerr << "Failed to read the database header" << std::endl;
            return 1;
        }

        unsigned int page_size =
            (static_cast<unsigned char>(buffer[0]) << 8) |
            static_cast<unsigned char>(buffer[1]);

        if (page_size == 1) {
            page_size = 65536;
        }

        std::cout << "database page size: " << page_size << std::endl;
    }

    return 0;
}
