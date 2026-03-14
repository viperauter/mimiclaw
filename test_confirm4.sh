#!/bin/bash

# Create a fifo for input
INPUT_FIFO="/tmp/mimiclaw_input_$$"
OUTPUT_LOG="/tmp/mimiclaw_output_$$.log"

# Clean up on exit
trap "rm -f $INPUT_FIFO; kill 0 2>/dev/null; exit" EXIT

# Create the fifo
mkfifo "$INPUT_FIFO"

# Keep the fifo open by running a background process that writes to it
# This prevents EOF from being sent when we close the write end
exec 3<>"$INPUT_FIFO"

# Start the mimiclaw application with input from fifo
./build/mimiclaw --logs debug > "$OUTPUT_LOG" 2>&1 < "$INPUT_FIFO" &

# Get the process ID
PID=$!

# Wait for the application to start
sleep 3

# Send the request to write a file
echo 'Write a file named test.txt with content "Hello, World!"' >&3

# Wait for the confirmation prompt to appear
sleep 5

# Send the confirmation response
echo '1' >&3

# Wait for the result
sleep 5

# Check if the file was created
if [ -f "test.txt" ]; then
    echo "Test passed: File test.txt was created"
    cat test.txt
else
    echo "Test failed: File test.txt was not created"
fi

# Show the output
echo "=== Application Output ==="
cat "$OUTPUT_LOG"

# Kill the application
kill $PID 2>/dev/null
wait $PID 2>/dev/null
