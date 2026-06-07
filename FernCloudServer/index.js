require('dotenv').config();
const express = require('express');
const multer = require('multer');
const path = require('path');
const fs = require('fs');
const fsPromises = require('fs').promises;
const cron = require('node-cron');
const { nanoid } = require('nanoid');

const app = express();
const PORT = process.env.PORT || 3000;
const UPLOADS_DIR = path.join(__dirname, 'uploads');
const API_KEY = process.env.API_KEY;

if (!fs.existsSync(UPLOADS_DIR)) fs.mkdirSync(UPLOADS_DIR);

const storage = multer.diskStorage({
    destination: UPLOADS_DIR,
    filename: (req, file, cb) => {
        const id = nanoid(6);
        cb(null, id + '.mp4'); // Force .mp4 as Fern generates .mp4
    }
});
const upload = multer({ 
    storage,
    limits: { fileSize: 500 * 1024 * 1024 } // 500 MB limit
});

const auth = (req, res, next) => {
    if (!API_KEY || req.headers['x-fern-api-key'] !== API_KEY) {
        return res.status(401).send('Unauthorized');
    }
    next();
};

app.post('/upload', auth, upload.single('clip'), (req, res) => {
    if (!req.file) return res.status(400).send('No file uploaded.');
    const id = path.basename(req.file.filename, '.mp4');
    res.json({ link: `${process.env.BASE_URL}/c/${id}` });
});

app.get('/c/:id', async (req, res) => {
    const id = req.params.id;
    const filePath = path.join(UPLOADS_DIR, id + '.mp4');
    try {
        await fsPromises.access(filePath);
        
        // If it's a direct request for the video file (like from a <video> tag or Discord)
        if (req.headers.accept && req.headers.accept.includes('video/')) {
            return res.sendFile(filePath);
        }

        // Otherwise, serve a simple HTML page with OpenGraph tags for Discord preview
        const videoUrl = `${process.env.BASE_URL}/c/${id}`;
        const html = `
<!DOCTYPE html>
<html>
<head>
    <title>Fern Clip</title>
    <meta property="og:type" content="video.other">
    <meta property="og:video" content="${videoUrl}">
    <meta property="og:video:secure_url" content="${videoUrl}">
    <meta property="og:video:type" content="video/mp4">
    <meta property="og:video:width" content="1280">
    <meta property="og:video:height" content="720">
    <meta property="og:title" content="Fern Clip">
    <meta property="og:site_name" content="Fern">
    <meta name="twitter:card" content="player">
    <meta name="twitter:player" content="${videoUrl}">
    <meta name="twitter:player:width" content="1280">
    <meta name="twitter:player:height" content="720">
    <style>
        body { background: #000; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }
        video { max-width: 100%; max-height: 100%; }
    </style>
</head>
<body>
    <video controls autoplay>
        <source src="${videoUrl}" type="video/mp4">
        Your browser does not support the video tag.
    </video>
</body>
</html>`;
        res.send(html);
    } catch {
        res.status(404).send('Clip not found or expired.');
    }
});

cron.schedule('0 * * * *', async () => {
    const now = Date.now();
    const expiry = 7 * 24 * 60 * 60 * 1000;
    try {
        const files = await fsPromises.readdir(UPLOADS_DIR);
        for (const file of files) {
            const filePath = path.join(UPLOADS_DIR, file);
            try {
                const stats = await fsPromises.stat(filePath);
                if (now - stats.mtimeMs > expiry) {
                    await fsPromises.unlink(filePath);
                    console.log(`Deleted expired clip: ${file}`);
                }
            } catch (err) {
                console.error(`Error processing file ${file}:`, err);
            }
        }
    } catch (err) {
        console.error('Error reading uploads directory:', err);
    }
});

if (!API_KEY || !process.env.BASE_URL) {
    console.error("WARNING: API_KEY or BASE_URL is not set in .env");
}

app.listen(PORT, () => console.log(`Fern Cloud running on port ${PORT}`));