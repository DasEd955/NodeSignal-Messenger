/* ===================================================================================
schema.sql -- Defines the SQLite Schema for Persisting Users & Messages
=================================================================================== */

-- Creates the Users Table
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT UNIQUE NOT NULL,
    created_at INTEGER NOT NULL
);

-- Creates the Messages Table
    -- sender_id links each message to the user who sent it
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sender_id INTEGER NOT NULL REFERENCES users(id),
    body TEXT NOT NULL,
    sent_at INTEGER NOT NULL
);
