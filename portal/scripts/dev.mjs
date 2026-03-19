import { spawn } from 'node:child_process';

const args = process.argv.slice(2);
const passthrough = [];
let forceDemo = false;

for (const arg of args) {
  if (arg === 'demo' || arg === '--demo') {
    forceDemo = true;
    continue;
  }
  passthrough.push(arg);
}

const env = {
  ...process.env,
  PORTAL_FORCE_DEMO: forceDemo ? '1' : (process.env.PORTAL_FORCE_DEMO || '')
};

if (forceDemo) {
  process.stdout.write('[portal] Starting dev server with forced demo mode\n');
}

const child = spawn('pnpm', ['exec', 'astro', 'dev', ...passthrough], {
  stdio: 'inherit',
  shell: false,
  env
});

child.on('exit', (code) => process.exit(code ?? 0));
