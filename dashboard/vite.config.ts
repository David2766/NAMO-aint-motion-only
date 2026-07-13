import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";

function appEntryForMode(mode: string): string {
  return mode === "demo" ? "/src/web/DemoShell.svelte" : "/src/web/App.svelte";
}

function dashboardBuildConfig(mode: string, base: string, outDir: string) {
  return {
    base,
    plugins: [svelte()],
    resolve: {
      alias: {
        "@app-entry": appEntryForMode(mode)
      }
    },
    build: {
      outDir,
      emptyOutDir: true,
      sourcemap: true,
      rollupOptions: {
        input: "index.html",
        output: {
          entryFileNames: "assets/dashboard.js",
          chunkFileNames: "assets/dashboard-[name].js",
          assetFileNames: (assetInfo: { name?: string }) => {
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

export default defineConfig(({ mode }) => {
  if (mode === "demo") {
    return dashboardBuildConfig(mode, "/NAMO-aint-motion-only/", "dist-demo");
  }

  if (mode === "web") {
    return dashboardBuildConfig(mode, "/dashboard/", "dist-web");
  }

  return {
    plugins: [svelte()],
    resolve: {
      alias: {
        "@app-entry": appEntryForMode(mode)
      }
    },
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
