// @ts-check
import { defineConfig } from "astro/config";
import tailwindcss from "@tailwindcss/vite";

export default defineConfig({
  site: "https://v3extender.pages.dev",
  output: "static",
  trailingSlash: "always",
  build: { format: "directory", inlineStylesheets: "auto" },
  devToolbar: { enabled: false },
  markdown: {
    shikiConfig: { theme: "github-dark-dimmed", wrap: false },
  },
  vite: { plugins: [tailwindcss()] },
});
