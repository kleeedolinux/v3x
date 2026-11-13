// @ts-check
import { defineConfig } from "astro/config";

export default defineConfig({
  site: "https://v3x.pages.dev",
  output: "static",
  trailingSlash: "always",
  build: { format: "directory", inlineStylesheets: "auto" },
  devToolbar: { enabled: false },
  markdown: {
    shikiConfig: { theme: "css-variables", wrap: false },
  },
});
