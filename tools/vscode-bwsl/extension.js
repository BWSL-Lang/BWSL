const vscode = require('vscode');
const childProcess = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const LANGUAGE_ID = 'bwsl';
const DIAGNOSTIC_OWNER = 'bwsl';

class BwslDiagnosticsController {
  constructor(context) {
    this.context = context;
    this.collection = vscode.languages.createDiagnosticCollection(DIAGNOSTIC_OWNER);
    this.output = vscode.window.createOutputChannel('BWSL Diagnostics');
    this.timers = new Map();
    this.children = new Map();
    this.generations = new Map();
    this.compilerWarningShown = false;

    context.subscriptions.push(this.collection, this.output);
  }

  activate() {
    this.context.subscriptions.push(
      vscode.workspace.onDidOpenTextDocument((document) => this.schedule(document, true)),
      vscode.workspace.onDidSaveTextDocument((document) => this.schedule(document, true)),
      vscode.workspace.onDidCloseTextDocument((document) => this.close(document)),
      vscode.workspace.onDidChangeTextDocument((event) => this.schedule(event.document, false)),
      vscode.workspace.onDidChangeConfiguration((event) => {
        if (event.affectsConfiguration('bwsl')) {
          this.validateOpenDocuments(true);
        }
      })
    );

    this.validateOpenDocuments(true);
  }

  validateOpenDocuments(immediate) {
    for (const document of vscode.workspace.textDocuments) {
      this.schedule(document, immediate);
    }
  }

  schedule(document, immediate) {
    if (!isBwslDocument(document)) return;

    const key = document.uri.toString();
    const existing = this.timers.get(key);
    if (existing) {
      clearTimeout(existing);
      this.timers.delete(key);
    }

    if (!this.isEnabled(document.uri)) {
      this.collection.delete(document.uri);
      this.stopRunningProcess(key);
      return;
    }

    const delay = immediate ? 0 : this.getDebounceMs(document.uri);
    const timer = setTimeout(() => {
      this.timers.delete(key);
      this.validate(document).catch((error) => {
        this.output.appendLine(`Unhandled diagnostics error: ${formatError(error)}`);
      });
    }, delay);
    this.timers.set(key, timer);
  }

  close(document) {
    const key = document.uri.toString();
    const timer = this.timers.get(key);
    if (timer) {
      clearTimeout(timer);
      this.timers.delete(key);
    }
    this.stopRunningProcess(key);
    this.collection.delete(document.uri);
    this.generations.delete(key);
  }

  async validate(document) {
    if (!isBwslDocument(document)) return;

    const key = document.uri.toString();
    const generation = (this.generations.get(key) || 0) + 1;
    this.generations.set(key, generation);
    this.stopRunningProcess(key);

    const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
    const config = vscode.workspace.getConfiguration('bwsl', document.uri);
    const compilerPath = this.resolveCompilerPath(config, workspaceFolder);
    const validationMode = config.get('diagnostics.validation', 'off');
    const timeoutMs = config.get('diagnostics.timeoutMs', 10000);

    let tempDir;
    try {
      tempDir = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'bwsl-vscode-'));
      const originalFile = document.uri.scheme === 'file' ? document.uri.fsPath : undefined;
      const inputFileName = originalFile ? path.basename(originalFile) : 'untitled.bwsl';
      const tempInput = path.join(tempDir, inputFileName);
      const tempOutput = path.join(tempDir, 'out');

      await fs.promises.mkdir(tempOutput, { recursive: true });
      await fs.promises.writeFile(tempInput, document.getText(), 'utf8');

      const args = [
        tempInput,
        '-errors-json',
        '-validation',
        validationMode,
        '-o',
        tempOutput
      ];

      for (const modulePath of this.collectModulePaths(config, workspaceFolder, originalFile)) {
        args.push('-modules', modulePath);
      }

      const result = await this.runCompiler(key, compilerPath, args, workspaceFolder, timeoutMs);
      if (this.generations.get(key) !== generation) return;

      if (result.launchError) {
        this.handleLaunchError(document, compilerPath, result.launchError);
        return;
      }

      const payload = parseCompilerJson(result.stdout);
      if (!payload) {
        this.output.appendLine(`bwslc did not produce diagnostics JSON for ${document.uri.fsPath || document.uri.toString()}`);
        if (result.stdout.trim()) this.output.appendLine(result.stdout.trim());
        if (result.stderr.trim()) this.output.appendLine(result.stderr.trim());
        this.collection.set(document.uri, [makeWholeFileDiagnostic(document, 'bwslc did not produce diagnostics JSON. See the BWSL Diagnostics output.')]);
        return;
      }

      this.collection.set(document.uri, diagnosticsFromPayload(payload, document));
    } finally {
      const child = this.children.get(key);
      if (child && child.killed) {
        this.children.delete(key);
      }
      if (tempDir) {
        await removeDirectory(tempDir);
      }
    }
  }

  runCompiler(key, compilerPath, args, workspaceFolder, timeoutMs) {
    return new Promise((resolve) => {
      const options = {
        cwd: workspaceFolder ? workspaceFolder.uri.fsPath : undefined,
        windowsHide: true,
        maxBuffer: 10 * 1024 * 1024,
        timeout: timeoutMs
      };

      const child = childProcess.execFile(compilerPath, args, options, (error, stdout, stderr) => {
        if (this.children.get(key) === child) {
          this.children.delete(key);
        }

        if (error && (error.code === 'ENOENT' || error.code === 'EACCES')) {
          resolve({ stdout, stderr, launchError: error });
          return;
        }

        if (error && error.killed && !stdout) {
          resolve({
            stdout,
            stderr,
            launchError: new Error(`bwslc timed out after ${timeoutMs} ms`)
          });
          return;
        }

        resolve({ stdout, stderr, launchError: undefined });
      });

      this.children.set(key, child);
    });
  }

  stopRunningProcess(key) {
    const child = this.children.get(key);
    if (!child) return;
    this.children.delete(key);
    child.kill();
  }

  handleLaunchError(document, compilerPath, error) {
    this.collection.delete(document.uri);
    this.output.appendLine(`Could not run ${compilerPath}: ${formatError(error)}`);
    if (!this.compilerWarningShown) {
      this.compilerWarningShown = true;
      vscode.window.showWarningMessage(`BWSL diagnostics could not run '${compilerPath}'. Set bwsl.compilerPath or build bwslc.`);
    }
  }

  isEnabled(uri) {
    return vscode.workspace.getConfiguration('bwsl', uri).get('diagnostics.enabled', true);
  }

  getDebounceMs(uri) {
    return vscode.workspace.getConfiguration('bwsl', uri).get('diagnostics.debounceMs', 400);
  }

  resolveCompilerPath(config, workspaceFolder) {
    const configuredPath = config.get('compilerPath', '').trim();
    if (configuredPath) {
      return resolveConfiguredPath(configuredPath, workspaceFolder);
    }

    const exeName = process.platform === 'win32' ? 'bwslc.exe' : 'bwslc';
    const candidates = [];
    if (workspaceFolder) {
      candidates.push(path.join(workspaceFolder.uri.fsPath, 'build', exeName));
      candidates.push(path.join(workspaceFolder.uri.fsPath, '..', 'build', exeName));
      candidates.push(path.join(workspaceFolder.uri.fsPath, '..', '..', 'build', exeName));
    }
    candidates.push(path.join(this.context.extensionPath, '..', '..', 'build', exeName));

    for (const candidate of candidates) {
      if (fileExists(candidate)) {
        return candidate;
      }
    }

    return exeName;
  }

  collectModulePaths(config, workspaceFolder, originalFile) {
    const paths = [];
    if (originalFile) {
      paths.push(path.dirname(originalFile));
    }
    if (workspaceFolder) {
      paths.push(workspaceFolder.uri.fsPath);
    }

    const configuredPaths = config.get('modulePaths', []);
    if (Array.isArray(configuredPaths)) {
      for (const configuredPath of configuredPaths) {
        if (typeof configuredPath === 'string' && configuredPath.trim()) {
          paths.push(resolveConfiguredPath(configuredPath.trim(), workspaceFolder));
        }
      }
    }

    return uniquePaths(paths);
  }
}

function activate(context) {
  const controller = new BwslDiagnosticsController(context);
  controller.activate();
}

function deactivate() {}

function isBwslDocument(document) {
  return document && document.languageId === LANGUAGE_ID;
}

function diagnosticsFromPayload(payload, document) {
  const items = Array.isArray(payload.diagnostics)
    ? payload.diagnostics
    : legacyErrorsToDiagnostics(payload.errors);

  if (items.length === 0 && payload.success === false) {
    return [makeWholeFileDiagnostic(document, 'BWSL compilation failed without diagnostic details.')];
  }

  return items.map((item) => toVsCodeDiagnostic(item, document));
}

function legacyErrorsToDiagnostics(errors) {
  if (!Array.isArray(errors)) return [];
  return errors.map((error) => {
    if (typeof error === 'string') {
      return { severity: 'error', message: error };
    }
    return {
      severity: 'error',
      message: error.message || 'BWSL compilation failed',
      line: error.line,
      column: error.column
    };
  });
}

function toVsCodeDiagnostic(item, document) {
  const diagnostic = new vscode.Diagnostic(
    rangeFromDiagnostic(item, document),
    formatDiagnosticMessage(item),
    severityFromString(item.severity)
  );
  diagnostic.source = item.phase ? `bwslc:${item.phase}` : 'bwslc';
  if (item.code) {
    diagnostic.code = item.code;
  }
  return diagnostic;
}

function formatDiagnosticMessage(item) {
  let message = item.message || 'BWSL diagnostic';
  const labels = [];
  if (item.pass) labels.push(item.pass);
  if (item.stage) labels.push(item.stage);
  if (labels.length > 0) {
    message = `[${labels.join('/')}] ${message}`;
  }
  return message;
}

function rangeFromDiagnostic(item, document) {
  if (!isPositiveInteger(item.line)) {
    return new vscode.Range(0, 0, 0, 0);
  }

  const startLine = clamp(item.line - 1, 0, Math.max(document.lineCount - 1, 0));
  const startCharacter = clamp((isPositiveInteger(item.column) ? item.column : 1) - 1, 0, document.lineAt(startLine).text.length);
  let endLine = startLine;
  let endCharacter = startCharacter + 1;

  if (isPositiveInteger(item.endLine) && isPositiveInteger(item.endColumn)) {
    endLine = clamp(item.endLine - 1, 0, Math.max(document.lineCount - 1, 0));
    endCharacter = clamp(item.endColumn - 1, 0, document.lineAt(endLine).text.length);
  } else if (Number.isInteger(item.length) && item.length > 0) {
    endCharacter = startCharacter + item.length;
  } else if (typeof item.token === 'string' && item.token.length > 0) {
    endCharacter = startCharacter + item.token.length;
  }

  endCharacter = clamp(endCharacter, 0, document.lineAt(endLine).text.length);
  if (endLine === startLine && endCharacter <= startCharacter) {
    endCharacter = clamp(startCharacter + 1, 0, document.lineAt(startLine).text.length);
  }

  return new vscode.Range(startLine, startCharacter, endLine, endCharacter);
}

function severityFromString(severity) {
  switch (severity) {
    case 'warning':
      return vscode.DiagnosticSeverity.Warning;
    case 'note':
      return vscode.DiagnosticSeverity.Information;
    case 'hint':
      return vscode.DiagnosticSeverity.Hint;
    case 'error':
    default:
      return vscode.DiagnosticSeverity.Error;
  }
}

function makeWholeFileDiagnostic(document, message) {
  return new vscode.Diagnostic(new vscode.Range(0, 0, 0, 0), message, vscode.DiagnosticSeverity.Error);
}

function parseCompilerJson(stdout) {
  const trimmed = stdout.trim();
  if (!trimmed) return undefined;
  try {
    return JSON.parse(trimmed);
  } catch (_) {
    const start = trimmed.indexOf('{');
    const end = trimmed.lastIndexOf('}');
    if (start >= 0 && end > start) {
      try {
        return JSON.parse(trimmed.slice(start, end + 1));
      } catch (_) {
        return undefined;
      }
    }
  }
  return undefined;
}

function resolveConfiguredPath(value, workspaceFolder) {
  let resolved = value;
  if (workspaceFolder) {
    resolved = resolved.replace(/\$\{workspaceFolder\}/g, workspaceFolder.uri.fsPath);
  }
  if (!path.isAbsolute(resolved) && workspaceFolder && hasPathSeparator(resolved)) {
    return path.resolve(workspaceFolder.uri.fsPath, resolved);
  }
  return resolved;
}

function hasPathSeparator(value) {
  return value.includes('/') || value.includes('\\');
}

function fileExists(filePath) {
  try {
    return fs.statSync(filePath).isFile();
  } catch (_) {
    return false;
  }
}

function uniquePaths(paths) {
  const seen = new Set();
  const result = [];
  for (const item of paths) {
    if (!item) continue;
    const normalized = path.normalize(item);
    const key = process.platform === 'win32' ? normalized.toLowerCase() : normalized;
    if (seen.has(key)) continue;
    seen.add(key);
    result.push(normalized);
  }
  return result;
}

async function removeDirectory(directory) {
  try {
    await fs.promises.rm(directory, { recursive: true, force: true });
  } catch (_) {
    // Temporary diagnostics files are best-effort cleanup.
  }
}

function isPositiveInteger(value) {
  return Number.isInteger(value) && value > 0;
}

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

function formatError(error) {
  return error && error.message ? error.message : String(error);
}

module.exports = {
  activate,
  deactivate
};
