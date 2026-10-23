import { readdir, readFile, mkdir, writeFile } from 'node:fs/promises'
import { join, relative } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = fileURLToPath(new URL('..', import.meta.url))
const publicDir = join(root, 'public')
const ignored = new Set(['node_modules', '.vitepress', 'public', 'scripts'])

async function walk(dir) {
  const entries = await readdir(dir, { withFileTypes: true })
  const files = []
  for (const entry of entries) {
    if (ignored.has(entry.name)) continue
    const path = join(dir, entry.name)
    if (entry.isDirectory()) files.push(...await walk(path))
    else if (entry.name.endsWith('.md')) files.push(path)
  }
  return files
}

function metadata(source) {
  const match = source.match(/^---\s*\n([\s\S]*?)\n---\s*\n/)
  const frontmatter = match?.[1] ?? ''
  const title = frontmatter.match(/^title:\s*(.+)$/m)?.[1]?.trim() ?? ''
  const description = frontmatter.match(/^description:\s*(.+)$/m)?.[1]?.trim() ?? ''
  return { title, description }
}

const files = (await walk(root)).sort()
const pages = []
const sections = []

for (const file of files) {
  const source = await readFile(file, 'utf8')
  const rel = relative(root, file).replaceAll('\\', '/')
  const slug = rel === 'index.md' ? '' : rel.slice(0, -3).replace(/\/index$/, '')
  const url = 'https://rkriad585.github.io/kirn/' + slug
  const { title, description } = metadata(source)
  const body = source.replace(/^---\s*\n[\s\S]*?\n---\s*\n/, '').trim()
  pages.push({ path: rel, url, title: title || rel, description, content: body })
  sections.push('- [' + (title || rel) + '](' + url + ')' + (description ? ' — ' + description : ''))
}

await mkdir(join(publicDir, 'ai'), { recursive: true })
await writeFile(join(publicDir, 'ai', 'docs.json'), JSON.stringify({
  name: 'Kirn Docs',
  description: 'Official documentation for the Kirn programming language.',
  version: '0.1.0',
  generatedAt: new Date().toISOString(),
  sourceRepository: 'https://github.com/rkriad585/kirn',
  website: 'https://rkriad585.github.io/kirn/',
  pages: pages.map(({ content, ...page }) => page)
}, null, 2) + '\n')

const index = '# Kirn\n\n> Official documentation for the Kirn programming language.\n\nKirn is a general-purpose language created by RK Riad Khan. These docs describe verified language behavior, tooling, and standard-library material from the source repository.\n\n## Documentation\n\n' + sections.join('\n') + '\n\n## Ecosystem\n\n- [Kirn website](https://kirn-lang.github.io/)\n- [Pets registry](https://pets-registry.github.io/)\n- [Kirnly editor](https://kirnly.github.io/)\n- [KirnOS](https://kirnos-kernel.github.io/)\n- [Creator](https://rkriad585.github.io/)\n'
await writeFile(join(publicDir, 'llms.txt'), index)

const full = pages.map(page => '# ' + page.title + '\n\nSource: ' + page.url + '\n\n' + (page.description ? page.description + '\n\n' : '') + page.content).join('\n\n---\n\n')
await writeFile(join(publicDir, 'llms-full.txt'), '# Kirn Docs — full content bundle\n\n' + full + '\n')
