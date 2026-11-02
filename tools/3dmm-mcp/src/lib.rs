//! MCP server for Microsoft 3D Movie Maker (3DMMEx).
//!
//! The app runs an automation bridge (newline-delimited JSON over
//! 127.0.0.1); this crate exposes it as MCP tools over stdio. See
//! doc/automation/NOTES.md in the repository for the protocol.

pub mod bridge;
pub mod launcher;
pub mod server;
pub mod speech;
pub mod tools;
pub mod video;
