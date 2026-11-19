import { execFile, spawn } from 'node:child_process'
import { mkdtemp, unlink, rmdir } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { createInterface } from 'node:readline'
import { fileURLToPath } from 'node:url'
import { promisify } from 'node:util'

const run = promisify(execFile)
const source = fileURLToPath(new URL('./InputHelper.cs', import.meta.url))
const systemRoot = process.env.SystemRoot || 'C:\\Windows'

export function windowsInput() {
  let directory
  let executable
  let build

  async function compile() {
    if (process.platform !== 'win32') throw new Error('Host input requires Windows.')
    directory = await mkdtemp(join(tmpdir(), 'evp-remote-input-'))
    executable = join(directory, 'InputHelper.exe')
    const csc = join(systemRoot, 'Microsoft.NET', 'Framework64', 'v4.0.30319', 'csc.exe')
    try {
      await run(csc, ['/nologo', '/noconfig', '/target:exe', '/platform:x64', `/out:${executable}`, source], {
        timeout: 10_000,
        windowsHide: true,
      })
    } catch (error) {
      const reason = error.code === 'ENOENT' ? 'The 64-bit .NET Framework C# compiler is unavailable.' :
        (error.stdout || error.stderr || error.message).trim()
      throw new Error(`The Windows input helper could not be built. ${reason}`)
    }
    return executable
  }

  return {
    async open({ signal }) {
      signal.throwIfAborted()
      build ??= compile()
      const path = await build
      signal.throwIfAborted()
      return startHelper(path, signal)
    },
    async dispose() {
      if (!directory) return
      try {
        await unlink(executable)
      } catch (error) {
        if (error.code !== 'ENOENT') throw error
      }
      await rmdir(directory)
    },
  }
}

async function startHelper(path, signal) {
  const child = spawn(path, [], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] })
  let details = ''
  child.once('error', (error) => { details = error.message })
  child.stdin.on('error', () => {})
  child.stderr.setEncoding('utf8').on('data', (chunk) => { details = (details + chunk).slice(0, 1024) })
  const closed = new Promise((resolve) => child.once('close', (code) => {
    resolve(details.trim() || (code === 0 ? '' : 'The Windows input helper stopped unexpectedly.'))
  }))
  const output = createInterface({ input: child.stdout })
  const close = () => {
    child.stdin.end()
    return closed
  }
  const cancel = () => child.kill()
  signal.addEventListener('abort', cancel, { once: true })
  const timer = setTimeout(cancel, 5000)

  try {
    const ready = new Promise((resolve, reject) => {
      output.once('line', (line) => {
        if (line === 'READY') resolve()
        else reject(new Error('The Windows input helper did not announce readiness.'))
      })
      closed.then((reason) => reject(new Error(reason || 'The Windows input helper closed before readiness.')))
    })
    await ready
    signal.throwIfAborted()
    return {
      write(command) {
        if (child.stdin.destroyed || !child.stdin.write(`${command}\n`)) {
          throw new Error('The Windows input pipe is unavailable.')
        }
      },
      close,
      closed,
    }
  } catch (error) {
    await close()
    throw error
  } finally {
    clearTimeout(timer)
    signal.removeEventListener('abort', cancel)
    output.close()
  }
}
