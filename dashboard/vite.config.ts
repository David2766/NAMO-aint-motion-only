import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";

export default defineConfig(({ mode }) => {
  if (mode === "web") {
    return {
      base: "/dashboard/",
      plugins: [svelte()],
      build: {
        outDir: "dist-web",
        emptyOutDir: true,
        sourcemap: true,
        rollupOptions: {
          input: "index.html",
          output: {
            entryFileNames: "assets/dashboard.js",
            chunkFileNames: "assets/dashboard-[name].js",
            assetFileNames: (assetInfo) => {
              if (assetInfo.name?.endsWith(".css")) {
                return "assets/dashboard.css";
              }
              return "assets/[name][extname]";
            }
          }
        }
      }
    };
  }

  return {
    plugins: [svelte()],
    build: {
      outDir: "dist",
      emptyOutDir: false,
      sourcemap: true,
      lib: {
        entry: "src/index-ha.ts",
        name: "RadarZoneCard",
        formats: ["es"],
        fileName: () => "radar-zone-card.js"
      },
      rollupOptions: {
        output: {
          codeSplitting: false
        }
      }
    }
  };
});
