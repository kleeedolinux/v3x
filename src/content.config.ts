import { defineCollection } from "astro:content";
import { glob } from "astro/loaders";
import { z } from "astro/zod";

// Filled by scripts/sync-docs.ts from the V3X repo. Do not edit the copies by hand.
const docs = defineCollection({
  loader: glob({ pattern: "*.md", base: "./src/content/docs" }),
  schema: z.object({
    title: z.string(),
    summary: z.string().default(""),
    group: z.enum(["players", "modders", "engine"]),
    order: z.number(),
    source: z.string(),
    sourceUrl: z.string(),
    updated: z.string(),
  }),
});

export const collections = { docs };
