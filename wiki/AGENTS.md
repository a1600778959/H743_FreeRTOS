# wiki/ Agent 指引

本目录是由微软 deep-wiki 流程生成的 VitePress 站点，承载仓库技术文档。

## Build & Run Commands（优先）

- 依赖安装：`cd wiki && npm install`
- 开发预览：`npm run dev`
- 构建静态站：`npm run build` → 输出 `.vitepress/dist/`
- 本地预览产物：`npm run preview`
- Windows 下 node v24+ / npm 11+ 已验证。

## Project Structure

- `index.md` — 面向开发者的 wiki 首页（禁止 hero/营销 frontmatter）
- `onboarding/` — 四份角色导读（contributor/staff/executive/PM）
- `NN-*/` — 编号分节的内容页
- `.vitepress/config.mts` — 站点配置（mermaid 暗色 themeVariables、sidebar）
- `.vitepress/theme/` — 缩放/专注模式/暗色主题
- `llms.txt` / `llms-full.txt` — LLM 友好索引与全文（`public/` 下有部署副本）

## Content Conventions

- Mermaid 一律暗色（theme variables 已覆盖；禁用 `<br/>`，用 `<br>`）；`sequenceDiagram` 必须带 `autonumber`
- 每个图表后跟 `<!-- Sources: path:line, ... -->`；声明性结论必须带 GitHub 链接引用（`main` 分支）
- 页面结构：Overview（为什么）→ at-a-glance 表 → 架构 → 数据流 → References → Related Pages
- 新页面必须同时登记到 `config.mts` 的 sidebar 与 `llms.txt`

## Boundaries

- ✅ 允许：新增内容页、更新过时引用、修正图表
- ⚠️ 先问：改主题 CSS/配置结构、改 sidebar 分组语义
- 🚫 禁止：删除已生成页面、引用不存在的源码路径、把 hero/营销文案引入 index.md

## Documentation

- 站点文档索引入口：`llms.txt`；全文合集：`llms-full.txt`
- 上游生成规范：microsoft/skills 仓库 `deep-wiki` 插件（commands/generate.md、build.md）
