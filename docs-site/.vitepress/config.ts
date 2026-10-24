import { defineConfig } from 'vitepress'
import { withMermaid } from 'vitepress-plugin-mermaid'

const kirnLanguage = {
  name: 'kirn',
  scopeName: 'source.kirn',
  displayName: 'Kirn',
  aliases: ['kn'],
  patterns: [
    { include: '#comments' },
    { include: '#strings' },
    { include: '#numbers' },
    { match: '\\b(fn|var|if|elif|else|while|for|in|return|break|continue|match|case|struct|enum|trait|impl|import|export|pub|defer|spawn|chan|select|try|catch|raise|unsafe|extern|new|box|self|Self|and|or|not|is|as|true|false|none|pass|class|interface|record|implements|extends|dynamic)\\b', name: 'keyword.control.kirn' },
    { match: '\\b(Int|UInt|Float|UFloat|Bool|Str|Char|BigInt|Rational|usize|isize)\\b', name: 'support.type.kirn' },
    { match: '\\b[A-Z][A-Za-z0-9_]*\\b', name: 'entity.name.type.kirn' },
    { match: '\\b[a-zA-Z_][a-zA-Z0-9_]*(?=\\s*\\()', name: 'entity.name.function.kirn' },
    { match: '->|<-|==|!=|<=|>=|\\.\\?\\.|\\.\\.=|\\.\\.|\\+|-|\\*|/|%|=|<|>|\\?', name: 'keyword.operator.kirn' }
  ],
  repository: {
    comments: {
      patterns: [{ match: '#.*$', name: 'comment.line.number-sign.kirn' }]
    },
    strings: {
      patterns: [{ begin: '"', end: '"', name: 'string.quoted.double.kirn', patterns: [{ match: '\\\\.', name: 'constant.character.escape.kirn' }] }, { begin: "'", end: "'", name: 'string.quoted.single.kirn' }]
    },
    numbers: {
      patterns: [{ match: '\\b[0-9]+(\\.[0-9]+)?\\b', name: 'constant.numeric.kirn' }]
    }
  }
}

export default withMermaid(defineConfig({
  title: 'Kirn Docs',
  description: 'The official documentation for the Kirn programming language.',
  lang: 'en-US',
  base: '/kirn/',
  cleanUrls: true,
  lastUpdated: true,
  appearance: true,
  mermaid: {
    theme: 'base',
    themeVariables: {
      primaryColor: '#dce6cc',
      primaryTextColor: '#101312',
      primaryBorderColor: '#759f37',
      lineColor: '#759f37',
      secondaryColor: '#e8e4dc',
      tertiaryColor: '#f2efe8'
    }
  },
  markdown: {
    languages: [kirnLanguage],
    theme: { light: 'github-light', dark: 'github-dark' }
  },
  themeConfig: {
    logo: 'https://kirn-lang.github.io/kirn-logos/kirn-icon.png',
    siteTitle: 'kirn / docs',
    nav: [
      { text: 'Learn', link: '/learn/introduction' },
      { text: 'Guides', link: '/guide/cli' },
      { text: 'Reference', link: '/reference/language/lexical' },
      { text: 'GitHub ↗', link: 'https://github.com/rkriad585/kirn' }
    ],
    sidebar: {
      '/learn/': [
        { text: 'Learn Kirn', items: [
          { text: 'Introduction', link: '/learn/introduction' },
          { text: 'Installation', link: '/learn/installation' },
          { text: 'Quick start', link: '/learn/quick-start' }
        ] },
        { text: 'Language basics', items: [
          { text: 'Variables and values', link: '/learn/basics/variables' },
          { text: 'Functions and control flow', link: '/learn/basics/functions' },
          { text: 'Structs and enums', link: '/learn/data/structs-enums' },
          { text: 'Optionals and results', link: '/learn/data/optionals-results' },
          { text: 'Concurrency', link: '/learn/beyond/concurrency' }
        ] }
      ],
      '/guide/': [
        { text: 'Guides', items: [
          { text: 'CLI workflow', link: '/guide/cli' },
          { text: 'Project structure', link: '/guide/project' },
          { text: 'Build targets', link: '/guide/toolchain' },
          { text: 'AI-readable docs', link: '/guide/ai' },
          { text: 'Contributing', link: '/guide/contributing' }
        ] }
      ],
      '/reference/': [
        { text: 'Language reference', items: [
          { text: 'Lexical structure', link: '/reference/language/lexical' },
          { text: 'Syntax and operators', link: '/reference/language/syntax' },
          { text: 'Types', link: '/reference/language/types' }
        ] },
        { text: 'Standard library', items: [
          { text: 'Overview', link: '/reference/stdlib/' },
          { text: 'core', link: '/reference/stdlib/core' },
          { text: 'io', link: '/reference/stdlib/io' },
          { text: 'math', link: '/reference/stdlib/math' },
          { text: 'strings', link: '/reference/stdlib/strings' },
          { text: 'collections', link: '/reference/stdlib/collections' },
          { text: 'json', link: '/reference/stdlib/json' },
          { text: 'os and path', link: '/reference/stdlib/os-path' },
          { text: 'regexp and time', link: '/reference/stdlib/regexp-time' }
        ] }
      ]
    },
    outline: { level: [2, 3] },
    search: { provider: 'local' },
    socialLinks: [
      { icon: 'github', link: 'https://github.com/rkriad585/kirn' }
    ],
    footer: {
      message: 'Built in the open by RK Riad Khan (RKRIAD585).',
      copyright: 'MIT licensed · <a href="https://kirn-lang.github.io/">Kirn website</a> · <a href="https://pets-registry.github.io/">Pets registry</a> · <a href="https://kirnly.github.io/">Kirnly</a> · <a href="https://kirnos-kernel.github.io/">KirnOS</a> · <a href="https://kirsh-shell.github.io/">Kirsh</a> · <a href="https://stirk-ai.github.io/">Stirk AI</a> · <a href="https://rkriad585.github.io/">Creator</a> · <a href="https://rkriad585.github.io/kirn/">Kirn docs</a> · <a href="https://pets-registry.github.io/pets">Pets docs</a>'
    },
    editLink: {
      pattern: 'https://github.com/rkriad585/kirn/edit/main/docs-site/:path',
      text: 'Edit this page on GitHub'
    }
  },
  head: [
    ['meta', { name: 'theme-color', content: '#101312' }],
    ['meta', { property: 'og:type', content: 'website' }],
    ['meta', { property: 'og:site_name', content: 'Kirn Docs' }],
    ['meta', { property: 'og:title', content: 'Kirn Docs — learn the Kirn programming language' }],
    ['meta', { property: 'og:description', content: 'Official guides and reference documentation for the Kirn programming language.' }],
    ['meta', { property: 'og:image', content: 'https://kirn-lang.github.io/kirn-logos/kirn-1280x640.png' }],
    ['link', { rel: 'icon', href: 'https://kirn-lang.github.io/kirn-logos/kirn-icon.png' }]
  ]
}))
