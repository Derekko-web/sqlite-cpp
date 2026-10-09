# SQLite database reader in C++

Maintained by **Derek Ko**.

A reader for SQLite database files that implements binary record decoding,
B-tree traversal, and a limited SQL query path in C++.

## Features

- Database information and table-name listing
- SQLite schema and record decoding
- Table and index B-tree traversal
- Selected-column queries and `COUNT(*)`
- Equality filters with index-based row lookup where supported

## Build and run

Requires CMake 3.13+ and a C++ compiler with C++23 support. No SQLite library
is required by the reader itself.

```sh
./your_program.sh sample.db .dbinfo
./your_program.sh sample.db .tables
```

Generate an independent demonstration database with Python's standard library:

```sh
python3 create_sample_database.py
./your_program.sh demo.db "SELECT name FROM projects WHERE category = 'systems'"
```

For a direct build:

```sh
cmake -S . -B build
cmake --build build
./build/sqlite sample.db .tables
```

Source code is in `src/main.cpp`. This is a read-only implementation with a
limited SQL subset; it does not implement general SQL execution or writes.
