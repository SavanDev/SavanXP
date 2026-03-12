const fs = require("fs");
const path = require("path");
const vscode = require("vscode");

const EXTENSION_SECTION = "savanxp";
const SDK_SNIPPETS_PATH = path.join("snippets", "sdk-snippets.json");
const PROJECT_MARKERS = [
    "build.ps1",
    path.join("tools", "build-user.ps1"),
    path.join("tools", "new-user-app.ps1"),
    path.join("sdk", "v1", "REFERENCE.md")
];

const EXAMPLE_DESCRIPTIONS = {
    errdemo: "Minimal stderr example.",
    fsdemo: "Create, write and read files under /disk.",
    hello: "Minimal stdout example.",
    multifile: "External app split across multiple sources.",
    pathops: "mkdir, rename, truncate, unlink and rmdir flow.",
    procpeek: "Inspect process table state.",
    spawnwait: "Launch a child process and wait for it.",
    statusdemo: "SDK metadata, statuses and error handling.",
    template: "Minimal starting point for new apps."
};

function activate(context) {
    const provider = new SavanXpExplorerProvider(context);
    const treeView = vscode.window.createTreeView("savanxpExplorer", {
        treeDataProvider: provider,
        showCollapseAll: true
    });

    const register = (command, handler) => {
        context.subscriptions.push(vscode.commands.registerCommand(command, handler));
    };

    register("savanxp.refreshExplorer", () => provider.refresh());
    register("savanxp.newSdkApp", async () => {
        const project = await resolveProjectContext();
        if (!project) {
            return;
        }

        const name = await vscode.window.showInputBox({
            prompt: "New SDK app name",
            placeHolder: "myapp",
            validateInput: validateAppName
        });
        if (!name) {
            return;
        }

        const scriptPath = path.join(project.rootPath, "tools", "new-user-app.ps1");
        const success = await executeTask(
            createPowerShellTask(project, {
                label: `SavanXP: New SDK App (${name})`,
                definition: { type: "savanxp", task: "new-app", name },
                scriptPath,
                scriptArgs: ["-Name", name],
                problemMatchers: []
            })
        );

        if (!success) {
            return;
        }

        provider.refresh();
        await openExampleByName(project, name);
    });

    register("savanxp.buildSdkApp", async (node) => buildSdkApp(node, { noInstall: false, runAfterBuild: false }));
    register("savanxp.buildSdkAppNoInstall", async (node) => buildSdkApp(node, { noInstall: true, runAfterBuild: false }));
    register("savanxp.runSdkApp", async (node) => buildSdkApp(node, { noInstall: false, runAfterBuild: true }));
    register("savanxp.openSdkReference", async () => {
        const project = await resolveProjectContext();
        if (!project) {
            return;
        }

        const referencePath = path.join(project.rootPath, "sdk", "v1", "REFERENCE.md");
        await openTextDocument(referencePath);
    });
    register("savanxp.buildBaseImage", async () => {
        const project = await resolveProjectContext();
        if (!project) {
            return;
        }

        const success = await buildBaseImage(project);
        if (!success) {
            return;
        }
        provider.refresh();
    });

    register("savanxp.openExample", async (node) => {
        const project = await resolveProjectContext();
        if (!project) {
            return;
        }
        const example = await resolveExampleSelection(project, node);
        if (!example) {
            return;
        }
        await openExampleMainFile(example.fullPath);
    });
    register("savanxp.openExampleFolder", async (node) => {
        const project = await resolveProjectContext();
        if (!project) {
            return;
        }
        const example = await resolveExampleSelection(project, node);
        if (!example) {
            return;
        }

        await vscode.commands.executeCommand("revealInExplorer", vscode.Uri.file(example.fullPath));
    });
    register("savanxp.buildExample", async (node) => buildSdkApp(node, { noInstall: false, runAfterBuild: false }));
    register("savanxp.buildExampleNoInstall", async (node) => buildSdkApp(node, { noInstall: true, runAfterBuild: false }));
    register("savanxp.runExample", async (node) => buildSdkApp(node, { noInstall: false, runAfterBuild: true }));
    register("savanxp.newAppFromExample", async (node) => {
        const project = await resolveProjectContext();
        if (!project) {
            return;
        }
        const example = await resolveExampleSelection(project, node);
        if (!example) {
            return;
        }

        const name = await vscode.window.showInputBox({
            prompt: `Create a new SDK app from ${example.name}`,
            placeHolder: `${example.name}-copy`,
            validateInput: validateAppName
        });
        if (!name) {
            return;
        }

        const destination = path.join(project.rootPath, "sdk", name);
        if (fs.existsSync(destination)) {
            vscode.window.showErrorMessage(`The destination already exists: ${destination}`);
            return;
        }

        fs.cpSync(example.fullPath, destination, { recursive: true });
        provider.refresh();
        await openExampleByName(project, name);
    });

    register("savanxp.previewSnippet", async (node) => {
        const snippetNode = resolveSnippetNode(node);
        if (!snippetNode) {
            return;
        }

        const content = [
            `# ${snippetNode.snippet.title}`,
            "",
            snippetNode.snippet.description,
            "",
            "```c",
            snippetNode.snippet.body,
            "```"
        ].join("\n");

        const document = await vscode.workspace.openTextDocument({
            language: "markdown",
            content
        });
        await vscode.window.showTextDocument(document, {
            preview: true,
            preserveFocus: false
        });
    });
    register("savanxp.insertSnippet", async (node) => {
        const snippetNode = resolveSnippetNode(node);
        if (!snippetNode) {
            return;
        }

        const editor = vscode.window.activeTextEditor;
        if (!editor) {
            vscode.window.showErrorMessage("Open a file before inserting a SavanXP snippet.");
            return;
        }

        await editor.insertSnippet(new vscode.SnippetString(snippetNode.snippet.body), editor.selection);
    });

    context.subscriptions.push(treeView);
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((event) => {
        if (event.affectsConfiguration(EXTENSION_SECTION)) {
            provider.refresh();
        }
    }));

    async function buildSdkApp(node, options) {
        const project = await resolveProjectContext();
        if (!project) {
            return;
        }

        const example = await resolveExampleSelection(project, node);
        if (!example) {
            return;
        }

        if (!options.noInstall) {
            const baseReady = await ensureBaseImage(project);
            if (!baseReady) {
                return;
            }
        }

        if (options.runAfterBuild) {
            const scriptPath = path.join(project.rootPath, "tools", "run-user.ps1");
            await executeTask(
                createPowerShellTask(project, {
                    label: `SavanXP: Run ${example.name}`,
                    definition: { type: "savanxp", task: "run-user", name: example.name },
                    scriptPath,
                    scriptArgs: [
                        "-Source", example.fullPath,
                        "-Name", example.name,
                        "-Destination", resolveDestination(example.name)
                    ],
                    problemMatchers: ["$gcc"]
                })
            );
            return;
        }

        const scriptPath = path.join(project.rootPath, "tools", "build-user.ps1");
        const args = [
            "-Source", example.fullPath,
            "-Name", example.name
        ];
        if (!options.noInstall) {
            args.push("-Destination", resolveDestination(example.name));
        } else {
            args.push("-NoInstall");
        }

        await executeTask(
            createPowerShellTask(project, {
                label: options.noInstall
                    ? `SavanXP: Build ${example.name} (No Install)`
                    : `SavanXP: Build ${example.name}`,
                definition: {
                    type: "savanxp",
                    task: options.noInstall ? "build-user-no-install" : "build-user",
                    name: example.name
                },
                scriptPath,
                scriptArgs: args,
                problemMatchers: ["$gcc"]
            })
        );
    }

    function resolveDestination(name) {
        const template = vscode.workspace.getConfiguration(EXTENSION_SECTION).get("defaultDestination", "/disk/bin/{name}");
        return template.includes("{name}") ? template.replace(/\{name\}/g, name) : template;
    }
}

function deactivate() {}

class SavanXpExplorerProvider {
    constructor(context) {
        this.context = context;
        this._onDidChangeTreeData = new vscode.EventEmitter();
        this.onDidChangeTreeData = this._onDidChangeTreeData.event;
        this.snippets = loadSnippetCatalog(context.extensionPath);
    }

    refresh() {
        this._onDidChangeTreeData.fire();
    }

    getTreeItem(element) {
        return element;
    }

    async getChildren(element) {
        const project = await resolveProjectContext({ quiet: true });
        if (!project) {
            if (element) {
                return [];
            }
            return [new InfoNode("Open the SavanXP repository root or configure `savanxp.projectRoot`.")];
        }

        if (!element) {
            return [
                new RootNode("Examples", "examplesRoot", "sdk examples", new vscode.ThemeIcon("folder-library")),
                new RootNode("SDK Snippets", "snippetRoot", "curated snippets", new vscode.ThemeIcon("symbol-snippet"))
            ];
        }

        if (element.kind === "examplesRoot") {
            const examples = listExamples(project.rootPath);
            if (examples.length === 0) {
                return [new InfoNode("No SDK examples found under sdk/.")];
            }
            return examples.map((example) => new ExampleNode(example));
        }

        if (element.kind === "snippetRoot") {
            const categories = getSnippetCategories(this.snippets);
            return categories.map((category) => new SnippetCategoryNode(category));
        }

        if (element.kind === "snippetCategory") {
            return this.snippets
                .filter((snippet) => snippet.category === element.category)
                .map((snippet) => new SnippetNode(snippet));
        }

        return [];
    }
}

class RootNode extends vscode.TreeItem {
    constructor(label, kind, description, iconPath) {
        super(label, vscode.TreeItemCollapsibleState.Expanded);
        this.kind = kind;
        this.contextValue = kind;
        this.description = description;
        this.iconPath = iconPath;
    }
}

class InfoNode extends vscode.TreeItem {
    constructor(label) {
        super(label, vscode.TreeItemCollapsibleState.None);
        this.kind = "info";
        this.contextValue = "info";
        this.iconPath = new vscode.ThemeIcon("info");
    }
}

class ExampleNode extends vscode.TreeItem {
    constructor(example) {
        super(example.name, vscode.TreeItemCollapsibleState.None);
        this.kind = "example";
        this.contextValue = "example";
        this.example = example;
        this.description = example.description;
        this.tooltip = `${example.name}\n${example.fullPath}`;
        this.iconPath = new vscode.ThemeIcon("file-directory");
        this.resourceUri = vscode.Uri.file(example.fullPath);
        this.command = {
            command: "savanxp.openExample",
            title: "Open Example",
            arguments: [this]
        };
    }
}

class SnippetCategoryNode extends vscode.TreeItem {
    constructor(category) {
        super(category, vscode.TreeItemCollapsibleState.Expanded);
        this.kind = "snippetCategory";
        this.category = category;
        this.contextValue = "snippetCategory";
        this.iconPath = new vscode.ThemeIcon("list-tree");
    }
}

class SnippetNode extends vscode.TreeItem {
    constructor(snippet) {
        super(snippet.title, vscode.TreeItemCollapsibleState.None);
        this.kind = "snippet";
        this.contextValue = "snippet";
        this.snippet = snippet;
        this.description = snippet.description;
        this.tooltip = `${snippet.title}\n${snippet.description}`;
        this.iconPath = new vscode.ThemeIcon("symbol-snippet");
        this.command = {
            command: "savanxp.previewSnippet",
            title: "Preview Snippet",
            arguments: [this]
        };
    }
}

function listExamples(rootPath) {
    const sdkRoot = path.join(rootPath, "sdk");
    if (!fs.existsSync(sdkRoot)) {
        return [];
    }

    return fs.readdirSync(sdkRoot, { withFileTypes: true })
        .filter((entry) => entry.isDirectory() && entry.name !== "v1")
        .map((entry) => ({
            name: entry.name,
            description: EXAMPLE_DESCRIPTIONS[entry.name] || "SDK example",
            fullPath: path.join(sdkRoot, entry.name)
        }))
        .sort((left, right) => left.name.localeCompare(right.name));
}

function getSnippetCategories(snippets) {
    return [...new Set(snippets.map((snippet) => snippet.category))]
        .sort((left, right) => left.localeCompare(right));
}

function loadSnippetCatalog(extensionPath) {
    const filePath = path.join(extensionPath, SDK_SNIPPETS_PATH);
    const raw = fs.readFileSync(filePath, "utf8");
    const snippets = JSON.parse(raw);
    return snippets.sort((left, right) => {
        const category = left.category.localeCompare(right.category);
        return category !== 0 ? category : left.title.localeCompare(right.title);
    });
}

function validateProjectRoot(rootPath) {
    if (!rootPath || !fs.existsSync(rootPath)) {
        return false;
    }
    return PROJECT_MARKERS.every((marker) => fs.existsSync(path.join(rootPath, marker)));
}

async function resolveProjectContext(options = {}) {
    const quiet = options.quiet === true;
    const configuration = vscode.workspace.getConfiguration(EXTENSION_SECTION);
    const configuredRoot = configuration.get("projectRoot", "").trim();
    if (configuredRoot) {
        if (!validateProjectRoot(configuredRoot)) {
            if (!quiet) {
                vscode.window.showWarningMessage(`Configured SavanXP project root is invalid: ${configuredRoot}`);
            }
            return null;
        }
        return toProjectContext(configuredRoot);
    }

    const folders = vscode.workspace.workspaceFolders || [];
    for (const folder of folders) {
        const resolved = findProjectRoot(folder.uri.fsPath);
        if (resolved) {
            return toProjectContext(resolved, folder);
        }
    }

    if (!quiet) {
        vscode.window.showWarningMessage("SavanXP repository not found in the current workspace.");
    }
    return null;
}

function toProjectContext(rootPath, workspaceFolder) {
    const folder = workspaceFolder || vscode.workspace.getWorkspaceFolder(vscode.Uri.file(rootPath));
    return {
        rootPath,
        workspaceFolder: folder || vscode.TaskScope.Workspace
    };
}

function findProjectRoot(startPath) {
    let current = path.resolve(startPath);
    while (true) {
        if (validateProjectRoot(current)) {
            return current;
        }

        const parent = path.dirname(current);
        if (parent === current) {
            return null;
        }
        current = parent;
    }
}

async function resolveExampleSelection(project, node) {
    if (node && node.example) {
        return node.example;
    }

    const active = resolveExampleFromActiveEditor(project.rootPath);
    if (active) {
        return active;
    }

    const examples = listExamples(project.rootPath);
    if (examples.length === 0) {
        vscode.window.showWarningMessage("No SDK examples are available under sdk/.");
        return null;
    }

    const picked = await vscode.window.showQuickPick(
        examples.map((example) => ({
            label: example.name,
            description: example.description,
            example
        })),
        { placeHolder: "Select a SavanXP SDK app" }
    );

    return picked ? picked.example : null;
}

function resolveExampleFromActiveEditor(rootPath) {
    const editor = vscode.window.activeTextEditor;
    if (!editor) {
        return null;
    }

    const sdkRoot = path.join(rootPath, "sdk");
    const filePath = editor.document.uri.fsPath;
    if (!filePath.startsWith(sdkRoot + path.sep)) {
        return null;
    }

    const relative = path.relative(sdkRoot, filePath);
    const parts = relative.split(path.sep);
    if (parts.length === 0 || parts[0] === "v1") {
        return null;
    }

    const fullPath = path.join(sdkRoot, parts[0]);
    if (!fs.existsSync(fullPath) || !fs.statSync(fullPath).isDirectory()) {
        return null;
    }

    return {
        name: parts[0],
        description: EXAMPLE_DESCRIPTIONS[parts[0]] || "SDK example",
        fullPath
    };
}

async function openExampleByName(project, name) {
    const examplePath = path.join(project.rootPath, "sdk", name);
    if (!fs.existsSync(examplePath)) {
        return;
    }
    await openExampleMainFile(examplePath);
}

async function openExampleMainFile(examplePath) {
    const mainPath = path.join(examplePath, "main.c");
    if (fs.existsSync(mainPath)) {
        await openTextDocument(mainPath);
        return;
    }

    const sourceFiles = fs.readdirSync(examplePath)
        .filter((entry) => entry.endsWith(".c") || entry.endsWith(".S"))
        .sort();
    if (sourceFiles.length === 0) {
        vscode.window.showWarningMessage(`No source files were found in ${examplePath}`);
        return;
    }

    await openTextDocument(path.join(examplePath, sourceFiles[0]));
}

async function openTextDocument(filePath) {
    const document = await vscode.workspace.openTextDocument(vscode.Uri.file(filePath));
    await vscode.window.showTextDocument(document, {
        preview: false,
        preserveFocus: false
    });
}

function resolveSnippetNode(node) {
    return node && node.snippet ? node : null;
}

function validateAppName(value) {
    if (!value || !value.trim()) {
        return "App name is required.";
    }
    if (!/^[A-Za-z0-9_-]+$/.test(value)) {
        return "Use letters, numbers, '-' or '_'.";
    }
    return null;
}

function createPowerShellTask(project, options) {
    const shellPath = vscode.workspace.getConfiguration(EXTENSION_SECTION).get("powershellPath", "powershell.exe");
    const args = [
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        options.scriptPath,
        ...options.scriptArgs
    ];
    const execution = new vscode.ShellExecution(shellPath, args, {
        cwd: project.rootPath
    });
    const task = new vscode.Task(
        options.definition,
        project.workspaceFolder,
        options.label,
        "SavanXP",
        execution,
        options.problemMatchers
    );
    task.presentationOptions = {
        reveal: vscode.TaskRevealKind.Always,
        panel: vscode.TaskPanelKind.Shared,
        clear: false
    };
    return task;
}

async function ensureBaseImage(project) {
    const diskImage = path.join(project.rootPath, "build", "disk.img");
    const autoBuild = vscode.workspace.getConfiguration(EXTENSION_SECTION).get("autoBuildBaseImage", true);
    if (fs.existsSync(diskImage) || !autoBuild) {
        return true;
    }

    return buildBaseImage(project);
}

async function buildBaseImage(project) {
    const scriptPath = path.join(project.rootPath, "build.ps1");
    return executeTask(
        createPowerShellTask(project, {
            label: "SavanXP: Build Base OS Image",
            definition: { type: "savanxp", task: "build-base-image" },
            scriptPath,
            scriptArgs: ["build"],
            problemMatchers: ["$gcc"]
        })
    );
}

async function executeTask(task) {
    const execution = await vscode.tasks.executeTask(task);
    return new Promise((resolve, reject) => {
        const disposable = vscode.tasks.onDidEndTaskProcess((event) => {
            if (event.execution !== execution) {
                return;
            }

            disposable.dispose();
            if (event.exitCode !== 0) {
                const detail = typeof event.exitCode === "number"
                    ? `exit code ${event.exitCode}`
                    : "no exit code";
                reject(new Error(`${task.name} failed with ${detail}`));
                return;
            }
            resolve(true);
        });
    }).catch((error) => {
        vscode.window.showErrorMessage(error.message);
        return false;
    });
}

module.exports = {
    activate,
    deactivate
};
