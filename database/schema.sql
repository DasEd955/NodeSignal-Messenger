-- schema.sql - Reference copy of the NodeSignal database schema.
--
-- This file is NOT the authoritative source. The canonical DDL lives in
-- database/db.c as the NS_SCHEMA_SQL string constant. If you change the
-- schema, update db.c first; then mirror the change here for reference.
--
-- DO NOT execute this file directly against a production database.
-- The server calls ns_db_init_schema() at startup, which applies NS_SCHEMA_SQL.

CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT UNIQUE NOT NULL,
    created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sender_id INTEGER NOT NULL REFERENCES users(id),
    body TEXT NOT NULL,
    sent_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_messages_sender_id ON messages (sender_id);
