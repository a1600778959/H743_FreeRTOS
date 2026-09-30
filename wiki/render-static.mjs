// 离线包预渲染：用本机 Chrome（CDP）逐页加载 wiki（base=/ 的正常路由），
// 等待 Mermaid SVG 渲染完成后保存 DOM，并按页面深度把站内绝对路径改写为
// 相对路径 —— 使解压后的站点在 file:// 下直接可见全部内容与图表。
import puppeteer from 'puppeteer-core'
import fs from 'node:fs'
import path from 'node:path'

const dist = path.resolve('.vitepress/dist')
const base = process.env.WIKI_ORIGIN || 'http://127.0.0.1:8931/'
// 浏览器可执行文件：WIKI_BROWSER > CHROME_PATH > 本机 Windows Chrome 默认路径
const BROWSER =
  process.env.WIKI_BROWSER ||
  process.env.CHROME_PATH ||
  'C:/Program Files/Google/Chrome/Application/chrome.exe'

const files = []
;(function walk(d) {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name)
    if (e.isDirectory()) walk(p)
    else if (e.name.endsWith('.html')) files.push(path.relative(dist, p).replace(/\\/g, '/'))
  }
})(dist)

const browser = await puppeteer.launch({
  executablePath: BROWSER,
  headless: true,
  args: ['--no-proxy-server', '--disable-gpu', '--no-first-run', '--disable-extensions'],
})
const page = await browser.newPage()
let ok = 0
let svgTotal = 0
const failed = []
for (const rel of files) {
  try {
    await page.goto(base + rel, { waitUntil: 'networkidle2', timeout: 30000 })
    await page
      .waitForFunction(
        () => {
          const nodes = document.querySelectorAll('.mermaid')
          if (nodes.length === 0) return true
          return [...nodes].every((el) => el.querySelector('svg'))
        },
        { timeout: 20000 },
      )
      .catch(() => {})
    await new Promise((r) => setTimeout(r, 250))
    let html = await page.content()
    // 按页面深度把站内绝对路径改写为相对路径（不含协议相对 //）
    const depth = rel.includes('/') ? rel.split('/').length - 1 : 0
    const prefix = '../'.repeat(depth)
    html = html.replace(/(href|src)="\/(?!\/)/g, `$1="${prefix}`)
    fs.writeFileSync(path.join(dist, rel), html)
    const svgs = (html.match(/<svg/g) || []).length
    svgTotal += svgs
    ok++
    console.log(`ok ${rel} depth=${depth} svg=${svgs}`)
    if (ok % 15 === 0) console.log('progress', ok, '/', files.length)
  } catch (e) {
    failed.push(rel + ' :: ' + e.message)
  }
}
await browser.close()
console.log(`done: ${ok}/${files.length} rendered, svg total=${svgTotal}`)
for (const f of failed) console.log('FAIL:', f)
