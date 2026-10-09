"""Create a small SQLite database for the reader's examples."""
import sqlite3
from pathlib import Path

path = Path(__file__).resolve().parent / "demo.db"
if path.exists():
    raise SystemExit(f"Refusing to overwrite {path}")
with sqlite3.connect(path) as connection:
    connection.executescript("""
        CREATE TABLE projects (id INTEGER PRIMARY KEY, name TEXT, category TEXT);
        CREATE INDEX projects_category ON projects(category);
        INSERT INTO projects (name, category) VALUES
            ('myos', 'systems'), ('interpreter-c', 'systems'), ('directory', 'web');
    """)
print(f"Created {path}")
