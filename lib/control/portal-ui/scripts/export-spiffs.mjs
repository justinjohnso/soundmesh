import fs from 'node:fs/promises';
import path from 'node:path';
import { gzipSync } from 'node:zlib';

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const distDir = path.join(root, 'dist');
const dataDir = path.resolve(root, '..', '..', '..', 'data');
const files = ['index.html', 'app.js', 'app.css'];

async function main() {
  await fs.mkdir(dataDir, { recursive: true });
  const existing = await fs.readdir(dataDir).catch(() => []);
  await Promise.all(existing.map((f) => fs.rm(path.join(dataDir, f), { force: true, recursive: true })));

  for (const file of files) {
    const src = path.join(distDir, file);
    const contents = await fs.readFile(src);
    const gz = gzipSync(contents, { level: 9 });
    await fs.writeFile(path.join(dataDir, `${file}.gz`), gz);
    process.stdout.write(`exported ${file}.gz (${gz.length} bytes)\n`);
  }

  process.stdout.write(`SPIFFS assets ready at: ${dataDir}\n`);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
