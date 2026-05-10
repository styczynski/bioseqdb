import fs from 'fs';
import path from 'path';
import { InputSource } from './types';

/**
 * Check if data is being piped via stdin
 */
export function hasStdin(): boolean {
  return !process.stdin.isTTY;
}

/**
 * Read from stdin
 */
export async function readStdin(): Promise<string> {
  return new Promise((resolve, reject) => {
    let data = '';

    process.stdin.setEncoding('utf8');

    process.stdin.on('data', (chunk) => {
      data += chunk;
    });

    process.stdin.on('end', () => {
      resolve(data);
    });

    process.stdin.on('error', (error) => {
      reject(error);
    });
  });
}

/**
 * Read from file
 */
export function readFile(filePath: string): string {
  const resolvedPath = path.resolve(filePath);

  if (!fs.existsSync(resolvedPath)) {
    throw new Error(`File not found: ${resolvedPath}`);
  }

  return fs.readFileSync(resolvedPath, 'utf8');
}

/**
 * Determine input source and read data
 */
export async function getInputSource(args: Array<string | number>): Promise<InputSource> {
  // Priority 1: Command line arguments (including files detected in cli.ts)
  if (args && args.length > 0) {
    return {
      type: 'args',
      data: args.map(String)
    };
  }

  // Priority 2: stdin
  if (hasStdin()) {
    return {
      type: 'stdin',
      data: await readStdin()
    };
  }

  throw new Error('No input provided. Use: chartscii <data> or chartscii data.json or echo data | chartscii');
}
