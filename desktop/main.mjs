import { execFile } from 'node:child_process'
import { join } from 'node:path'
import { parseArgs, promisify } from 'node:util'
import { startDesktop } from './server.mjs'
import { windowsInput } from './windows.mjs'

const run = promisify(execFile)

async function main() {
  const { values } = parseArgs({
    options: {
      'no-open': { type: 'boolean' },
      port: { type: 'string', default: '0' },
    },
  })
  const port = Number(values.port)
  if (!/^\d+$/.test(values.port) || !Number.isInteger(port) || port > 65535) {
    throw new Error('--port must be an integer from 0 to 65535.')
  }

  const native = windowsInput()
  const desktop = await startDesktop({ createInput: native.open, port, signalUrl: process.env.EVP_SIGNAL_URL })
  async function shutdown() {
    await desktop.close()
    await native.dispose()
  }
  for (const signal of ['SIGINT', 'SIGTERM', 'SIGBREAK']) {
    process.once(signal, () => shutdown().catch((error) => {
      console.error(error.message)
      process.exitCode = 1
    }))
  }

  if (!values['no-open'] && process.platform === 'win32') {
    try {
      const rundll = join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'rundll32.exe')
      await run(rundll, ['url.dll,FileProtocolHandler', desktop.launchUrl], { windowsHide: true })
      console.log(`Desktop UI listening on ${new URL(desktop.launchUrl).origin}`)
      return
    } catch (error) {
      console.error(`The default browser could not be opened. ${error.message}`)
    }
  }
  console.log(desktop.launchUrl)
}

main().catch((error) => {
  console.error(error.message)
  process.exitCode = 1
})
