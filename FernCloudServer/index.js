require('dotenv').config();
const express = require('express');
const multer = require('multer');
const path = require('path');
const fs = require('fs');
const fsPromises = require('fs').promises;
const { execFile } = require('child_process');
const cron = require('node-cron');
const { nanoid } = require('nanoid');

const app = express();
const PORT = process.env.PORT || 3000;
const UPLOADS_DIR = path.join(__dirname, 'uploads');
const API_KEY = process.env.API_KEY;
const CLIP_EXTENSION = '.mp4';
const THUMB_EXTENSION = '.jpg';
const EXPIRY_MS = 7 * 24 * 60 * 60 * 1000;

if (!fs.existsSync(UPLOADS_DIR)) fs.mkdirSync(UPLOADS_DIR);

const baseUrl = () => (process.env.BASE_URL || '').replace(/\/+$/, '');
const clipUrl = (id) => `${baseUrl()}/c/${encodeURIComponent(id)}`;
const videoUrl = (id) => `${baseUrl()}/v/${encodeURIComponent(id)}${CLIP_EXTENSION}`;
const thumbnailUrl = (id) => `${baseUrl()}/thumb/${encodeURIComponent(id)}${THUMB_EXTENSION}`;
const clipPath = (id) => path.join(UPLOADS_DIR, `${id}${CLIP_EXTENSION}`);
const metadataPath = (id) => path.join(UPLOADS_DIR, `${id}.json`);
const thumbnailPath = (id) => path.join(UPLOADS_DIR, `${id}${THUMB_EXTENSION}`);
const isClipId = (id) => /^[A-Za-z0-9_-]+$/.test(id);

const escapeHtml = (value) => String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');

const cleanTitle = (value) => String(value || '')
    .replace(/[\u0000-\u001f\u007f]/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, 160);

const deriveTitle = (fileName, id) => {
    const baseName = path.basename(fileName || '', path.extname(fileName || ''));
    const title = cleanTitle(baseName.replace(/_export_\d{8}_\d{6}(?: \(\d+\))?$/i, ''));
    return title || `Clip ${id}`;
};

const readMetadata = async (id) => {
    try {
        const json = await fsPromises.readFile(metadataPath(id), 'utf8');
        return JSON.parse(json);
    } catch {
        return {};
    }
};

const writeMetadata = async (id, file, title) => {
    const metadata = {
        id,
        title: cleanTitle(title) || deriveTitle(file.originalname, id),
        originalName: cleanTitle(file.originalname),
        uploadedAt: new Date().toISOString()
    };

    await fsPromises.writeFile(metadataPath(id), JSON.stringify(metadata, null, 2), 'utf8');
    return metadata;
};

const fileExists = async (filePath) => {
    try {
        await fsPromises.access(filePath);
        return true;
    } catch {
        return false;
    }
};

const ensureThumbnail = async (id) => {
    const outputPath = thumbnailPath(id);
    if (await fileExists(outputPath)) return true;

    return new Promise((resolve) => {
        const child = execFile(
            'ffmpeg',
            [
                '-hide_banner',
                '-y',
                '-ss', '00:00:01',
                '-i', clipPath(id),
                '-frames:v', '1',
                '-vf', 'scale=1280:-1',
                '-q:v', '4',
                outputPath
            ],
            { timeout: 5000 },
            (err) => resolve(!err)
        );

        child.on('error', () => resolve(false));
    });
};

const storage = multer.diskStorage({
    destination: UPLOADS_DIR,
    filename: (req, file, cb) => {
        const id = nanoid(6);
        cb(null, id + CLIP_EXTENSION); // Force .mp4 as Fern generates .mp4
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

app.post('/upload', auth, upload.single('clip'), async (req, res) => {
    if (!req.file) return res.status(400).send('No file uploaded.');
    const id = path.basename(req.file.filename, CLIP_EXTENSION);

    try {
        const metadata = await writeMetadata(id, req.file, req.body?.title);
        ensureThumbnail(id).catch((err) => console.error(`Error generating thumbnail for clip ${id}:`, err));
        res.json({ id, link: videoUrl(id), pageLink: clipUrl(id), title: metadata.title });
    } catch (err) {
        console.error(`Error saving metadata for clip ${id}:`, err);
        res.status(500).send('Error saving clip metadata.');
    }
});

app.get('/list', auth, async (req, res) => {
    try {
        const files = await fsPromises.readdir(UPLOADS_DIR);
        const clips = await Promise.all(files
            .filter((file) => path.extname(file).toLowerCase() === CLIP_EXTENSION)
            .map(async (file) => {
                const filePath = path.join(UPLOADS_DIR, file);
                const stats = await fsPromises.stat(filePath);
                const id = path.basename(file, CLIP_EXTENSION);
                const metadata = await readMetadata(id);
                const title = cleanTitle(metadata.title) || deriveTitle(metadata.originalName || file, id);
                return {
                    id,
                    link: videoUrl(id),
                    pageLink: clipUrl(id),
                    name: file,
                    title,
                    timestamp: stats.mtime,
                    size: stats.size
                };
            }));
        // Sort by newest first
        clips.sort((a, b) => b.timestamp - a.timestamp);
        res.json(clips);
    } catch (err) {
        res.status(500).send('Error listing clips.');
    }
});

app.delete('/clip/:id', auth, async (req, res) => {
    const id = req.params.id;
    if (!isClipId(id)) return res.status(400).send('Invalid clip id.');

    const filePath = clipPath(id);
    try {
        await fsPromises.unlink(filePath);
        await fsPromises.unlink(metadataPath(id)).catch(() => {});
        await fsPromises.unlink(thumbnailPath(id)).catch(() => {});
        res.json({ deleted: true });
    } catch (err) {
        if (err.code === 'ENOENT') return res.status(404).send('Clip not found or expired.');
        console.error(`Error deleting clip ${id}:`, err);
        res.status(500).send('Error deleting clip.');
    }
});

app.get('/v/:id.mp4', async (req, res) => {
    const id = req.params.id;
    if (!isClipId(id)) return res.status(400).send('Invalid clip id.');

    try {
        await fsPromises.access(clipPath(id));
        return res.sendFile(clipPath(id));
    } catch {
        res.status(404).send('Clip not found or expired.');
    }
});

app.get('/thumb/:id.jpg', async (req, res) => {
    const id = req.params.id;
    if (!isClipId(id)) return res.status(400).send('Invalid clip id.');

    try {
        if (!await ensureThumbnail(id)) {
            return res.status(404).send('Thumbnail not available.');
        }

        return res.sendFile(thumbnailPath(id));
    } catch {
        res.status(404).send('Thumbnail not available.');
    }
});

app.get('/c/:id', async (req, res) => {
    const id = req.params.id;
    if (!isClipId(id)) return res.status(400).send('Invalid clip id.');

    const filePath = clipPath(id);
    try {
        await fsPromises.access(filePath);
        const metadata = await readMetadata(id);
        const title = cleanTitle(metadata.title) || deriveTitle(metadata.originalName, id);
        
        // If it's a direct request for the video file (like from a <video> tag or Discord)
        if (req.headers.accept && req.headers.accept.includes('video/')) {
            return res.sendFile(filePath);
        }

        // Otherwise, serve a simple HTML page with OpenGraph tags for Discord preview
        const pageUrl = clipUrl(id);
        const directVideoUrl = videoUrl(id);
        const directThumbnailUrl = thumbnailUrl(id);
        const escapedTitle = escapeHtml(title);
        const escapedPageUrl = escapeHtml(pageUrl);
        const escapedDirectVideoUrl = escapeHtml(directVideoUrl);
        const escapedThumbnailUrl = escapeHtml(directThumbnailUrl);
        const html = `
<!DOCTYPE html>
<html>
<head>
    <title>${escapedTitle}</title>
    <meta property="og:url" content="${escapedPageUrl}">
    <meta property="og:type" content="video.other">
    <meta property="og:title" content="${escapedTitle}">
    <meta property="og:description" content="Clip partagé avec Fern">
    <meta property="og:site_name" content="Fern">
    <meta property="og:image" content="${escapedThumbnailUrl}">
    <meta property="og:image:secure_url" content="${escapedThumbnailUrl}">
    <meta property="og:image:type" content="image/jpeg">
    <meta property="og:video" content="${escapedDirectVideoUrl}">
    <meta property="og:video:url" content="${escapedDirectVideoUrl}">
    <meta property="og:video:secure_url" content="${escapedDirectVideoUrl}">
    <meta property="og:video:type" content="video/mp4">
    <meta property="og:video:width" content="1280">
    <meta property="og:video:height" content="720">
    <meta name="twitter:card" content="player">
    <meta name="twitter:title" content="${escapedTitle}">
    <meta name="twitter:image" content="${escapedThumbnailUrl}">
    <meta name="twitter:player" content="${escapedPageUrl}">
    <meta name="twitter:player:stream" content="${escapedDirectVideoUrl}">
    <meta name="twitter:player:stream:content_type" content="video/mp4">
    <meta name="twitter:player:width" content="1280">
    <meta name="twitter:player:height" content="720">
    <style>
        body { background: #000; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }
        video { max-width: 100%; max-height: 100%; }
    </style>
</head>
<body>
    <video controls autoplay poster="${escapedThumbnailUrl}">
        <source src="${escapedDirectVideoUrl}" type="video/mp4">
        Your browser does not support the video tag.
    </video>
</body>
</html>`;
        res.type('html').send(html);
    } catch {
        res.status(404).send('Clip not found or expired.');
    }
});

cron.schedule('0 * * * *', async () => {
    const now = Date.now();
    try {
        const files = await fsPromises.readdir(UPLOADS_DIR);
        for (const file of files) {
            if (path.extname(file).toLowerCase() !== CLIP_EXTENSION) continue;

            const filePath = path.join(UPLOADS_DIR, file);
            try {
                const stats = await fsPromises.stat(filePath);
                if (now - stats.mtimeMs > EXPIRY_MS) {
                    const id = path.basename(file, CLIP_EXTENSION);
                    await fsPromises.unlink(filePath);
                    await fsPromises.unlink(metadataPath(id)).catch(() => {});
                    await fsPromises.unlink(thumbnailPath(id)).catch(() => {});
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
