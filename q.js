const WebSocket = require('ws');
const http = require('http');
const express = require('express');
const path = require('path');
const mongoose = require('mongoose');

// =================================================================
// Database Setup (MongoDB)
// =================================================================
// ** UPDATED: New MongoDB Connection String **
const MONGODB_URI = "mongodb+srv://atifsupermart202199:FGzi4j6kRnYTIyP9@cluster0.bfulggv.mongodb.net/?retryWrites=true&w=majority";

mongoose.connect(MONGODB_URI)
    .then(() => console.log('Successfully connected to MongoDB.'))
    .catch(err => console.error('MongoDB connection error:', err));

const motorLogSchema = new mongoose.Schema({
    onTime: { type: Date, required: true },
    offTime: { type: Date },
    duration: { type: String }
});

const MotorLog = mongoose.model('MotorLog', motorLogSchema);


// =================================================================
// Server Setup
// =================================================================
const app = express();
const server = http.createServer(app);
app.use(express.static(path.join(__dirname, 'public')));
const wss = new WebSocket.Server({ server });

let esp32Client = null;
const webClients = new Set();
let lastKnownMotorStatus = 'OFF';

wss.on('connection', (ws) => {
    console.log('A client connected.');

    ws.on('message', async (message) => {
        let data;
        try { data = JSON.parse(message); } catch (e) { return; }

        // --- Message handling logic ---
        
        if (data.type === 'esp32-identify') {
            console.log('ESP32 client identified.');
            esp32Client = ws;
            webClients.forEach(client => client.send(JSON.stringify({ type: 'espStatus', status: 'online' })));
        
        } else if (data.type === 'statusUpdate' && ws === esp32Client) {
            const newStatus = data.payload.motorStatus;

            if (newStatus !== lastKnownMotorStatus) {
                console.log(`Motor status changed from ${lastKnownMotorStatus} to ${newStatus}`);
                if (newStatus === 'ON') {
                    const newLog = new MotorLog({ onTime: new Date() });
                    await newLog.save();
                    console.log('New log created for ON event.');
                } else { // Status is now OFF
                    const lastLog = await MotorLog.findOne({ offTime: null }).sort({ onTime: -1 });
                    if (lastLog) {
                        lastLog.offTime = new Date();
                        const durationSeconds = Math.round((lastLog.offTime - lastLog.onTime) / 1000);
                        lastLog.duration = formatDuration(durationSeconds);
                        await lastLog.save();
                        console.log('Log updated for OFF event. Duration:', lastLog.duration);
                    }
                }
                lastKnownMotorStatus = newStatus;
            }
            webClients.forEach(client => client.send(message.toString()));

        } else if (data.type === 'command' && (data.command === 'GET_ALL_LOGS' || data.command === 'GET_LOG_PAGE')) {
            const logs = await MotorLog.find().sort({ onTime: -1 }).limit(100);
            const formattedLogs = logs.map(log => ({
                onTime: new Date(log.onTime).toLocaleString('en-BD', { timeZone: 'Asia/Dhaka' }),
                offTime: log.offTime ? new Date(log.offTime).toLocaleString('en-BD', { timeZone: 'Asia/Dhaka' }) : 'Running...',
                duration: log.duration || 'Running...'
            }));
            ws.send(JSON.stringify({ type: 'allLogsUpdate', payload: { motorLogs: formattedLogs } }));

        } else if (data.type === 'command') {
            if (esp32Client) {
                esp32Client.send(message.toString());
            }
        } else {
             if (!webClients.has(ws)) {
                webClients.add(ws);
                const espStatus = esp32Client ? 'online' : 'offline';
                ws.send(JSON.stringify({ type: 'espStatus', status: espStatus }));
             }
        }
    });

    ws.on('close', () => {
        if (ws === esp32Client) {
            console.log('ESP32 client disconnected.');
            esp32Client = null;
            webClients.forEach(client => client.send(JSON.stringify({ type: 'espStatus', status: 'offline' })));
        } else {
            webClients.delete(ws);
            console.log('Web client disconnected.');
        }
    });
});

function formatDuration(totalSeconds) {
    if (totalSeconds < 60) return `${totalSeconds}s`;
    const hours = Math.floor(totalSeconds / 3600);
    totalSeconds %= 3600;
    const minutes = Math.floor(totalSeconds / 60);
    const seconds = totalSeconds % 60;
    let durationString = "";
    if (hours > 0) durationString += `${hours}h `;
    if (minutes > 0) durationString += `${minutes}m `;
    durationString += `${seconds}s`;
    return durationString;
}

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => console.log(`Server is listening on port ${PORT}`));