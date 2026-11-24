const { MarkdownView, Notice, Platform, Plugin } = require("obsidian");
const crypto = require("crypto");
const fs = require("fs/promises");
const path = require("path");

module.exports = class MurmurSync extends Plugin {
    onload() {
        if (!Platform.isWin) return;
        this.loaded = true;
        const run = () => this.requestSync();
        this.addRibbonIcon("refresh-cw", "同步到掌机", run);
        this.addCommand({ id: "sync", name: "同步到掌机", callback: run });
    }

    onunload() {
        this.loaded = false;
        this.progress?.hide();
    }

    async requestSync() {
        if (this.progress) return;
        this.progress = new Notice("正在提交到树莓派…", 0);
        try {
            const view = this.app.workspace.getActiveViewOfType(MarkdownView);
            if (view) await view.save();
            const vault = await fs.realpath(this.app.vault.adapter.getBasePath());
            const key = crypto.createHash("sha256").update(path.normalize(vault).toLowerCase()).digest("hex");
            const folder = path.join(process.env.LOCALAPPDATA, "Murmur", "manual-sync", key);
            await fs.mkdir(folder, { recursive: true });
            const id = crypto.randomBytes(16).toString("hex");
            const receipt = path.join(folder, id + ".result");
            await fs.writeFile(path.join(folder, id + ".request"), "", { flag: "wx" });
            const end = Date.now() + 60000;
            while (this.loaded && Date.now() < end) {
                let message;
                try {
                    message = await fs.readFile(receipt, "utf8");
                } catch (err) {
                    if (err.code !== "ENOENT") throw err;
                }
                if (message) {
                    if (this.loaded) new Notice(message, 6500);
                    await fs.rm(receipt, { force: true }).catch(() => {});
                    return;
                }
                await new Promise(resolve => setTimeout(resolve, 500));
            }
            if (this.loaded) new Notice("已发送同步请求，尚未收到电脑后台服务的回执。", 6500);
        } catch (err) {
            console.error("Murmur manual sync", err);
            if (this.loaded) new Notice("未能提交或读取同步结果，请检查本机 Murmur 同步服务。", 6500);
        } finally {
            this.progress.hide();
            this.progress = null;
        }
    }
};
