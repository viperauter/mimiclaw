#!/bin/bash
# WeChat iLink API Test Script

BASE_URL="https://ilinkai.weixin.qq.com"
BOT_TYPE=3

echo "=== WeChat iLink API Test ==="
echo ""

# Step 1: Get QR Code
echo "Step 1: Getting QR Code..."
RESPONSE=$(curl -s "${BASE_URL}/ilink/bot/get_bot_qrcode?bot_type=${BOT_TYPE}")

echo "Response:"
echo "${RESPONSE}" | python3 -m json.tool 2>/dev/null || echo "${RESPONSE}"

# Extract qrcode and qrcode_img_content
QRCODE=$(echo "${RESPONSE}" | python3 -c "
import json
import sys
data = json.load(sys.stdin)
print(data.get('qrcode', ''))
" 2>/dev/null)

QR_IMG_URL=$(echo "${RESPONSE}" | python3 -c "
import json
import sys
data = json.load(sys.stdin)
print(data.get('qrcode_img_content', ''))
" 2>/dev/null)

if [ -z "${QRCODE}" ]; then
    echo "ERROR: Failed to get qrcode"
    exit 1
fi

echo ""
echo "QR Code ID: ${QRCODE}"
echo "QR Image URL: ${QR_IMG_URL}"
echo ""
echo "Please open the URL above in your browser and scan the QR code with WeChat."
echo ""

# Step 2: Poll for QR status
echo "Step 2: Polling for QR status (press Ctrl+C to stop)..."
echo ""

while true; do
    STATUS_RESPONSE=$(curl -s -H "iLink-App-ClientVersion: 1" \
        "${BASE_URL}/ilink/bot/get_qrcode_status?qrcode=${QRCODE}")
    
    STATUS=$(echo "${STATUS_RESPONSE}" | python3 -c "
import json
import sys
data = json.load(sys.stdin)
print(data.get('status', 'unknown'))
" 2>/dev/null)

    echo "[$(date '+%H:%M:%S')] Status: ${STATUS}"
    
    if [ "${STATUS}" = "confirmed" ]; then
        echo ""
        echo "Login successful! Full response:"
        echo "${STATUS_RESPONSE}" | python3 -m json.tool 2>/dev/null || echo "${STATUS_RESPONSE}"
        
        # Extract token
        BOT_TOKEN=$(echo "${STATUS_RESPONSE}" | python3 -c "
import json
import sys
data = json.load(sys.stdin)
print(data.get('bot_token', ''))
" 2>/dev/null)
        
        BOT_ID=$(echo "${STATUS_RESPONSE}" | python3 -c "
import json
import sys
data = json.load(sys.stdin)
print(data.get('ilink_bot_id', ''))
" 2>/dev/null)
        
        echo ""
        echo "Bot Token: ${BOT_TOKEN}"
        echo "Bot ID: ${BOT_ID}"
        break
    elif [ "${STATUS}" = "expired" ]; then
        echo ""
        echo "QR Code expired! Please run again."
        exit 1
    fi
    
    sleep 2
done