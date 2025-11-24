// server.js
// =================================================================
// Simple & Robust WebSocket Relay Server for ESP32
// =================================================================

const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const path = require('path');

const app = express();
// 'public' ফোল্ডারে আপনার HTML ফাইলটি রাখুন
app.use(express.static(path.join(__dirname, 'public')));

const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

let esp32Socket = null; // ESP32 এর কানেকশন রাখার জন্য

wss.on('connection', (ws) => {
    console.log('A new client connected.');

    ws.on('message', (message) => {
        let data;
        try {
            // gelen mesajın string olup olmadığını kontrol et
            data = JSON.parse(message.toString());
        } catch (e) {
            console.error('Failed to parse JSON:', e);
            return;
        }

        // Identify the client type
        if (data.type === 'esp32-identify') {
            console.log('ESP32 device connected.');
            esp32Socket = ws;
            ws.isEsp32 = true;
        } 
        else if (data.type === 'statusUpdate' && ws.isEsp32) {
            // ESP32 থেকে স্ট্যাটাস আসলে, সকল ড্যাশবোর্ড ক্লায়েন্টকে পাঠানো হবে
            wss.clients.forEach((client) => {
                if (!client.isEsp32 && client.readyState === WebSocket.OPEN) {
                    client.send(JSON.stringify({ type: 'statusUpdate', payload: data.payload }));
                }
            });
        }
        else if (data.type === 'command') {
            // ড্যাশবোর্ড থেকে কমান্ড আসলে, ESP32-কে পাঠানো হবে
            if (esp32Socket && esp32Socket.readyState === WebSocket.OPEN) {
                console.log('Forwarding command to ESP32:', data);
                esp32Socket.send(JSON.stringify(data));
            }
        }
    });

    ws.on('close', () => {
        console.log('Client disconnected.');
        if (ws.isEsp32) {
            esp32Socket = null;
            console.log('ESP32 device disconnected.');
        }
    });
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
    console.log(`Server is listening on port ${PORT}`);
});