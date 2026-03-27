# Debugging Guide

## Log File Management

### Basic Logging

Run the application with log output redirected to a file:

```bash
./build/mimiclaw -f logs/mimiclaw.log
```

### Timestamp-Based Log Files

To create a new log file for each debugging session with a timestamp:

```bash
./build/mimiclaw -f logs/$(date +%Y%m%d_%H%M%S).log
```

This will generate filenames like:
- `logs/20260327_143522.log`
- `logs/20260327_144015.log`

### Create Logs Directory

First, ensure the logs directory exists:

```bash
mkdir -p logs
```

### Viewing Logs

#### Real-time Log Monitoring

```bash
tail -f logs/20260327_143522.log
```

#### Filter Logs by Level

```bash
# Error logs only
grep " E " logs/20260327_143522.log

# Warning logs
grep " W " logs/20260327_143522.log

# Info logs
grep " I " logs/20260327_143522.log

# Debug logs
grep " D " logs/20260327_143522.log
```

#### Filter by Module

```bash
# Network-related logs
grep "tag=NET" logs/20260327_143522.log

# Agent-related logs
grep "tag=AGENT" logs/20260327_143522.log
```

## Log Levels

The application supports four log levels:

| Level | Tag | Description |
|-------|-----|-------------|
| ERROR | `E` | Critical errors |
| WARN | `W` | Warning messages |
| INFO | `I` | General information (default) |
| DEBUG | `D` | Detailed debugging information |

## Log File Format

Each log entry follows this format:

```
HH:MM:SS.ms L tag=TAG file=filename.c line=NN: message
```

Example:
```
14:35:22.123 I tag=APP file=app.c line=125: Agent started successfully
14:35:22.456 E tag=NET file=network.c line=78: Connection failed: timeout
```

## Common Debugging Workflows

### 1. Start a New Debug Session

```bash
mkdir -p logs
./build/mimiclaw -f logs/$(date +%Y%m%d_%H%M%S).log
```

### 2. View Recent Logs

```bash
# View last 100 lines
tail -n 100 logs/20260327_143522.log

# View logs from the last hour
grep "^14:[0-5][0-9]" logs/20260327_143522.log
```

### 3. Analyze Errors

```bash
# Count errors by type
grep " E " logs/20260327_143522.log | awk '{print $5}' | sort | uniq -c

# View errors with context
grep -A 2 -B 2 " E " logs/20260327_143522.log
```

### 4. Clean Up Old Logs

```bash
# Delete logs older than 7 days
find logs/ -name "*.log" -mtime +7 -delete

# Keep only the last 10 log files
ls -t logs/*.log | tail -n +11 | xargs rm -f
```

---

*Last updated: 2026-03-27*
