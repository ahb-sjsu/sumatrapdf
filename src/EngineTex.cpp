/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/ScopedWin.h"
#include "base/File.h"
#include "base/DirScan.h"
#include "base/GuessFileType.h"
#include "base/Win.h"

#include "gui/UIModels.h"

#include "DocProperties.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"

Kind kindEngineTex = "engineTex";

// magic comments ("% !TEX root = main.tex") and \documentclass sit near the top
constexpr int kTexHeaderBytes = 8 * 1024;
// a sibling .tex is read whole to see whether it \input{}s the opened file
constexpr i64 kMaxSiblingTexSize = 4 * 1024 * 1024;
// latex, latex after the bibliography, then reruns for cross-references / toc
constexpr int kMaxLatexPasses = 4;
constexpr DWORD kDefaultTimeoutMs = 5 * 60 * 1000;
constexpr int kMaxErrorMsgLen = 300;
// the home-page thumbnail loads a document again right after the tab does;
// that second load reuses the compile instead of running latex twice
constexpr u64 kReuseWindowMs = 60 * 1000;

// only these are ever launched, whatever a "% !TEX program" comment says
static const Str kTexPrograms[] = {StrL("pdflatex"), StrL("xelatex"), StrL("lualatex")};
static const Str kDefaultTexProgram = StrL("pdflatex");
static const Str kPdfLatexExe = StrL("pdflatex.exe");

static Mutex gTexMutex;
static Str gTexBinDir;    // dir with pdflatex.exe, cached once found
static Str gTexErrorPath; // file whose last compile failed
static Str gTexError;     // why it failed

struct TexLastBuild {
    Str path; // opened file
    Str root;
    Str pdf;
    FILETIME pathMod{};
    FILETIME rootMod{};
    u64 doneTick = 0;
};
static TexLastBuild gLastBuild; // guarded by gTexMutex

static bool HasPdfLatex(Str dir) {
    if (len(dir) == 0) {
        return false;
    }
    TempStr exe = path::JoinTemp(dir, kPdfLatexExe);
    return file::Exists(exe);
}

// newest C:\texlive\<year>\bin\windows (bin\win32 before TeX Live 2023)
static TempStr FindTexLiveBinDirTemp() {
    TempStr sysDrive = GetEnvVariableTemp(StrL("SystemDrive"));
    if (!sysDrive) {
        sysDrive = StrL("C:");
    }
    TempStr texlive = path::JoinTemp(sysDrive, StrL("texlive"));
    StrVec years;
    DirIter di(texlive);
    di.includeFiles = false;
    di.includeDirs = true;
    for (DirIterEntry* e : di) {
        years.Append(e->name);
    }
    SortNatural(&years);
    for (int i = len(years) - 1; i >= 0; i--) {
        TempStr bin = path::JoinTemp(texlive, years[i], StrL("bin"));
        TempStr dir = path::JoinTemp(bin, StrL("windows"));
        if (HasPdfLatex(dir)) {
            return dir;
        }
        dir = path::JoinTemp(bin, StrL("win32"));
        if (HasPdfLatex(dir)) {
            return dir;
        }
    }
    return {};
}

// SUMATRAPDF_TEX_BIN, then %PATH%, then the default MiKTeX and TeX Live locations
static TempStr FindTexBinDirTemp() {
    TempStr dir = GetEnvVariableTemp(StrL("SUMATRAPDF_TEX_BIN"));
    if (HasPdfLatex(dir)) {
        return dir;
    }

    TempStr envPath = GetEnvVariableTemp(StrL("PATH"));
    StrVec paths;
    Split(&paths, envPath, StrL(";"), true);
    for (Str p : paths) {
        if (HasPdfLatex(p)) {
            return str::DupTemp(p);
        }
    }

    TempStr localAppData = GetEnvVariableTemp(StrL("LOCALAPPDATA"));
    dir = path::JoinTemp(localAppData, StrL("Programs\\MiKTeX\\miktex\\bin\\x64"));
    if (HasPdfLatex(dir)) {
        return dir;
    }
    TempStr programFiles = GetEnvVariableTemp(StrL("ProgramFiles"));
    dir = path::JoinTemp(programFiles, StrL("MiKTeX\\miktex\\bin\\x64"));
    if (HasPdfLatex(dir)) {
        return dir;
    }

    return FindTexLiveBinDirTemp();
}

// don't free the result
static Str TexBinDir() {
    gTexMutex.Lock();
    Str dir = gTexBinDir;
    gTexMutex.Unlock();
    if (dir) {
        return dir;
    }

    TempStr found = FindTexBinDirTemp();
    if (!found) {
        return {};
    }
    gTexMutex.Lock();
    if (!gTexBinDir) {
        gTexBinDir = str::Dup(found);
    }
    dir = gTexBinDir;
    gTexMutex.Unlock();
    return dir;
}

static void RememberTexError(Str path, Str msg) {
    gTexMutex.Lock();
    str::Free(gTexErrorPath);
    str::Free(gTexError);
    gTexErrorPath = str::Dup(path);
    gTexError = str::Dup(msg);
    gTexMutex.Unlock();
    if (msg) {
        logf("EngineTex: '%s': %s\n", path, msg);
    }
}

// whitespace-trimmed view; str::TrimWSInPlace() writes into the buffer
static Str TrimView(Str s) {
    int start = 0;
    int end = s.len;
    while (start < end && str::IsWs(s.s[start])) {
        start++;
    }
    while (end > start && str::IsWs(s.s[end - 1])) {
        end--;
    }
    return Str(s.s + start, end - start);
}

static Str SkipView(Str s, int n) {
    return Str(s.s + n, s.len - n);
}

// "% !TEX root = ../main.tex" => key "root", value "../main.tex"
static bool ParseMagicComment(Str line, Str& key, Str& val) {
    line = TrimView(line);
    if (!str::StartsWith(line, StrL("%"))) {
        return false;
    }
    line = TrimView(SkipView(line, 1));
    if (!str::StartsWithI(line, StrL("!TEX"))) {
        return false;
    }
    line = SkipView(line, 4);
    int eq = str::IndexOfChar(line, '=');
    if (eq < 0) {
        return false;
    }
    key = TrimView(Str(line.s, eq));
    val = TrimView(SkipView(line, eq + 1));
    return len(key) > 0 && len(val) > 0;
}

struct TexHeaderInfo {
    TempStr root;    // "% !TEX root", {} if absent
    TempStr program; // "% !TEX program" or TeXShop's "TS-program", {} if absent
    bool isRoot = false;
    bool wantsUnicodeEngine = false; // fontspec needs xelatex or lualatex
};

static TempStr ReadHeaderTemp(Str path) {
    char* buf = AllocArrayTemp<char>(kTexHeaderBytes + 1);
    int n = file::ReadN(path, (u8*)buf, kTexHeaderBytes);
    if (n <= 0) {
        return {};
    }
    return TempStr(buf, n);
}

static TexHeaderInfo ParseTexHeader(Str hdr) {
    TexHeaderInfo res;
    res.isRoot = str::ContainsI(hdr, StrL("\\documentclass")) || str::Contains(hdr, StrL("\\begin{document}"));
    res.wantsUnicodeEngine = str::Contains(hdr, StrL("{fontspec}"));
    Str line;
    Str rest = hdr;
    while (str::NextLine(rest, line, rest)) {
        Str key, val;
        if (!ParseMagicComment(line, key, val)) {
            continue;
        }
        if (str::EqI(key, StrL("root"))) {
            res.root = str::DupTemp(val);
        } else if (str::EqI(key, StrL("program")) || str::EqI(key, StrL("TS-program"))) {
            res.program = str::DupTemp(val);
        }
    }
    return res;
}

// does a root pull in the opened file: \input{ch1}, \include{ch1.tex}, \input{chapters/ch1}
static bool ReferencesFile(Str content, Str baseNoExt) {
    TempStr a = fmt("{%s}", baseNoExt);
    TempStr b = fmt("{%s.tex}", baseNoExt);
    TempStr c = fmt("/%s}", baseNoExt);
    TempStr d = fmt("/%s.tex}", baseNoExt);
    return str::ContainsI(content, a) || str::ContainsI(content, b) || str::ContainsI(content, c) ||
           str::ContainsI(content, d);
}

// A chapter file has no \documentclass. Its root is the sibling .tex that has
// one and includes it, or the only sibling that has one.
static TempStr FindRootInDirTemp(Str path) {
    TempStr dir = path::GetDirTemp(path);
    TempStr base = path::GetBaseNameTemp(path);
    TempStr baseNoExt = path::GetPathNoExtTemp(base);
    TempStr onlyRoot;
    int nRoots = 0;
    for (DirIterEntry* e : DirIter(dir)) {
        if (!e->isFile || !str::EndsWithI(e->name, StrL(".tex")) || str::EqI(e->name, base)) {
            continue;
        }
        if (e->size > kMaxSiblingTexSize) {
            continue;
        }
        Str content = file::ReadFile(e->filePath);
        bool isRoot = str::ContainsI(content, StrL("\\documentclass"));
        bool refs = isRoot && ReferencesFile(content, baseNoExt);
        str::Free(content);
        if (!isRoot) {
            continue;
        }
        if (refs) {
            return str::DupTemp(e->filePath);
        }
        nRoots++;
        onlyRoot = str::DupTemp(e->filePath);
    }
    if (nRoots == 1) {
        return onlyRoot;
    }
    return {};
}

// the .tex handed to latex: a "% !TEX root" comment wins, then the file itself
// if it has \documentclass, then a sibling root that includes it
static TempStr ResolveTexRootTemp(Str path, const TexHeaderInfo& hdr) {
    if (hdr.root) {
        TempStr root = hdr.root;
        if (!path::IsAbsolute(root)) {
            root = path::JoinTemp(path::GetDirTemp(path), root);
        }
        root = path::NormalizeTemp(root);
        if (file::Exists(root)) {
            return root;
        }
        logf("EngineTex: TEX root file '%s' not found\n", root);
    }
    if (hdr.isRoot) {
        return str::DupTemp(path);
    }
    TempStr sibling = FindRootInDirTemp(path);
    if (sibling) {
        return sibling;
    }
    return str::DupTemp(path);
}

static Str PickTexProgram(const TexHeaderInfo& hdr) {
    for (Str p : kTexPrograms) {
        if (str::EqI(hdr.program, p)) {
            return p;
        }
    }
    if (hdr.program) {
        logf("EngineTex: ignoring unknown TEX program '%s'\n", hdr.program);
    }
    if (hdr.wantsUnicodeEngine) {
        return StrL("xelatex");
    }
    return kDefaultTexProgram;
}

// %TEMP%\SumatraPDF-tex\<hash of root path>: stable across recompiles so the
// .aux is reused, distinct per document
static TempStr TexTempOutDirTemp(Str root) {
    u32 h = 5381;
    for (int i = 0; i < root.len; i++) {
        h = h * 33 + (u8)root.s[i];
    }
    TempStr dir = path::JoinTemp(GetTempDirTemp(), StrL("SumatraPDF-tex"), fmt("%x", h));
    dir::CreateAll(dir);
    return dir;
}

struct TexBuild {
    Str path;    // file the user opened
    Str root;    // .tex handed to latex
    Str rootDir; // latex runs here so \input and .bib resolve
    Str outDir;  // .aux / .log / .pdf; rootDir unless it is read-only
    Str program;
    Str binDir;
};

static bool SeparateOutDir(const TexBuild& b) {
    return !str::EqI(b.outDir, b.rootDir);
}

static TempStr TexBaseNameTemp(const TexBuild& b) {
    return path::GetPathNoExtTemp(path::GetBaseNameTemp(b.root));
}

static TempStr TexOutFileTemp(const TexBuild& b, Str ext) {
    return path::JoinTemp(b.outDir, str::JoinTemp(TexBaseNameTemp(b), ext));
}

static TempStr TexExeTemp(const TexBuild& b, Str name) {
    return path::JoinTemp(b.binDir, str::JoinTemp(name, StrL(".exe")));
}

enum class RunResult {
    Ok,
    LaunchFailed,
    TimedOut,
    Failed
};

static DWORD TexTimeoutMs() {
    TempStr s = GetEnvVariableTemp(StrL("SUMATRAPDF_TEX_TIMEOUT_SECS"));
    if (!s) {
        return kDefaultTimeoutMs;
    }
    int secs = 0;
    if (!str::Parse(s, "%d%$", &secs)) {
        return kDefaultTimeoutMs;
    }
    if (secs <= 0) {
        return INFINITE;
    }
    return (DWORD)secs * 1000;
}

static RunResult RunTexTool(Str cmdLine, Str dir) {
    logf("EngineTex: %s (in '%s')\n", cmdLine, dir);
    HANDLE process = LaunchProcessInDir(cmdLine, dir, CREATE_NO_WINDOW);
    if (!process) {
        return RunResult::LaunchFailed;
    }
    AutoCloseHandle closer(process);
    DWORD wait = WaitForSingleObject(process, TexTimeoutMs());
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process, 1);
        return RunResult::TimedOut;
    }
    DWORD exitCode = EXIT_FAILURE;
    GetExitCodeProcess(process, &exitCode);
    return exitCode == EXIT_SUCCESS ? RunResult::Ok : RunResult::Failed;
}

// nonstopmode + halt-on-error: never waits for a keypress, stops at the first
// error. -synctex=1 gives double-click-to-source via PdfSync
static RunResult RunLatex(const TexBuild& b) {
    TempStr exe = TexExeTemp(b, b.program);
    TempStr outArg;
    if (SeparateOutDir(b)) {
        outArg = fmt(" -output-directory=\"%s\"", b.outDir);
    }
    TempStr cmd = fmt("\"%s\" -interaction=nonstopmode -halt-on-error -file-line-error -synctex=1%s \"%s\"", exe,
                      outArg, path::GetBaseNameTemp(b.root));
    return RunTexTool(cmd, b.rootDir);
}

enum class BibTool {
    None,
    BibTeX,
    Biber
};

// biblatex + biber leave a .bcf; classic \bibliography leaves \bibdata in the .aux
static BibTool NeedsBibTool(const TexBuild& b) {
    if (file::Exists(TexOutFileTemp(b, StrL(".bcf")))) {
        return BibTool::Biber;
    }
    Str aux = file::ReadFile(TexOutFileTemp(b, StrL(".aux")));
    bool bibtex = str::Contains(aux, StrL("\\bibdata{")) && str::Contains(aux, StrL("\\citation{"));
    str::Free(aux);
    return bibtex ? BibTool::BibTeX : BibTool::None;
}

static RunResult RunBibTool(const TexBuild& b, BibTool tool) {
    TempStr base = TexBaseNameTemp(b);
    if (tool == BibTool::Biber) {
        TempStr exe = TexExeTemp(b, StrL("biber"));
        TempStr dirs;
        if (SeparateOutDir(b)) {
            dirs = fmt(" --input-directory=\"%s\" --output-directory=\"%s\"", b.outDir, b.outDir);
        }
        TempStr cmd = fmt("\"%s\"%s \"%s\"", exe, dirs, base);
        return RunTexTool(cmd, b.rootDir);
    }

    TempStr exe = TexExeTemp(b, StrL("bibtex"));
    if (!SeparateOutDir(b)) {
        TempStr cmd = fmt("\"%s\" \"%s\"", exe, base);
        return RunTexTool(cmd, b.rootDir);
    }
    // the .aux is in outDir, the .bib files next to the source (MiKTeX flag)
    TempStr cmd = fmt("\"%s\" -include-directory=\"%s\" \"%s\"", exe, b.rootDir, base);
    return RunTexTool(cmd, b.outDir);
}

static bool LogWantsRerun(const TexBuild& b) {
    Str log = file::ReadFile(TexOutFileTemp(b, StrL(".log")));
    bool rerun = str::Contains(log, StrL("Rerun to get")) || str::ContainsI(log, StrL("Rerun LaTeX"));
    str::Free(log);
    return rerun;
}

// "./main.tex:12: Undefined control sequence." (-file-line-error format)
static bool IsFileLineError(Str line) {
    int idx = str::IndexOf(line, StrL(".tex:"));
    if (idx < 0) {
        return false;
    }
    Str rest = SkipView(line, idx + 5);
    int n = 0;
    while (n < rest.len && rest.s[n] >= '0' && rest.s[n] <= '9') {
        n++;
    }
    return n > 0 && n < rest.len && rest.s[n] == ':';
}

// first error in the .log plus the "l.12 \foo" line that follows it
static TempStr TexLogErrorTemp(Str logPath) {
    Str log = file::ReadFile(logPath);
    if (!log) {
        return {};
    }
    defer {
        str::Free(log);
    };
    TempStr res;
    Str line;
    Str rest = log;
    int linesAfter = 0;
    while (str::NextLine(rest, line, rest)) {
        if (res) {
            if (str::StartsWith(line, StrL("l."))) {
                res = str::JoinTemp(res, StrL(" "), line);
                break;
            }
            if (++linesAfter > 6) {
                break;
            }
            continue;
        }
        if (str::StartsWith(line, StrL("!")) || IsFileLineError(line)) {
            res = str::DupTemp(line);
        }
    }
    if (len(res) > kMaxErrorMsgLen) {
        res.len = kMaxErrorMsgLen;
    }
    return res;
}

static void RememberFailure(const TexBuild& b, RunResult r) {
    TempStr logPath = TexOutFileTemp(b, StrL(".log"));
    TempStr msg;
    switch (r) {
        case RunResult::LaunchFailed:
            msg = fmt("Could not start %s", TexExeTemp(b, b.program));
            break;
        case RunResult::TimedOut:
            msg = fmt("%s did not finish in time (SUMATRAPDF_TEX_TIMEOUT_SECS raises the limit)", b.program);
            break;
        default:
            msg = TexLogErrorTemp(logPath);
            if (!msg) {
                msg = fmt("%s failed, see %s", b.program, logPath);
            }
    }
    RememberTexError(b.path, msg);
}

// the PDF of the last compile of path if that was moments ago, its sources
// have not been touched since and it is still there; {} otherwise
static TempStr ReusableTexPdfTemp(Str path) {
    gTexMutex.Lock();
    TempStr res;
    TexLastBuild& b = gLastBuild;
    bool fresh = str::EqI(path, b.path) && GetTickCount64() - b.doneTick < kReuseWindowMs;
    if (fresh && file::Exists(b.pdf)) {
        FILETIME pathMod = file::GetModificationTime(b.path);
        FILETIME rootMod = file::GetModificationTime(b.root);
        if (FileTimeEq(pathMod, b.pathMod) && FileTimeEq(rootMod, b.rootMod)) {
            res = str::DupTemp(b.pdf);
        }
    }
    gTexMutex.Unlock();
    return res;
}

static void RememberBuild(Str path, Str root, Str pdf) {
    gTexMutex.Lock();
    TexLastBuild& b = gLastBuild;
    str::Free(b.path);
    str::Free(b.root);
    str::Free(b.pdf);
    b.path = str::Dup(path);
    b.root = str::Dup(root);
    b.pdf = str::Dup(pdf);
    b.pathMod = file::GetModificationTime(path);
    b.rootMod = file::GetModificationTime(root);
    b.doneTick = GetTickCount64();
    gTexMutex.Unlock();
}

// latex, the bibliography tool if anything is cited, latex again, then reruns
// while the log asks for them. Returns the PDF path, {} on failure (the reason
// is kept for EngineTexLastErrorTemp)
static TempStr CompileTexTemp(Str path) {
    Str binDir = TexBinDir();
    if (!binDir) {
        return {};
    }
    TempStr reused = ReusableTexPdfTemp(path);
    if (reused) {
        logf("EngineTex: reusing '%s'\n", reused);
        return reused;
    }

    TexHeaderInfo hdr = ParseTexHeader(ReadHeaderTemp(path));
    TempStr root = ResolveTexRootTemp(path, hdr);
    TexHeaderInfo rootHdr = hdr;
    if (!path::IsSame(root, path)) {
        rootHdr = ParseTexHeader(ReadHeaderTemp(root));
        if (!rootHdr.program) {
            rootHdr.program = hdr.program;
        }
    }

    TexBuild b;
    b.path = path;
    b.root = root;
    b.rootDir = path::GetDirTemp(root);
    b.outDir = b.rootDir;
    if (!dir::HasWriteAccess(b.rootDir)) {
        b.outDir = TexTempOutDirTemp(root);
    }
    b.program = PickTexProgram(rootHdr);
    b.binDir = binDir;

    int nPasses = 1;
    RunResult r = RunLatex(b);
    if (r != RunResult::Ok) {
        RememberFailure(b, r);
        return {};
    }

    BibTool bib = NeedsBibTool(b);
    if (bib != BibTool::None) {
        // a broken .bib still leaves a readable paper with [?] citations
        if (RunBibTool(b, bib) != RunResult::Ok) {
            logf("EngineTex: bibliography tool failed, continuing\n");
        }
        nPasses++;
        r = RunLatex(b);
        if (r != RunResult::Ok) {
            RememberFailure(b, r);
            return {};
        }
    }

    while (nPasses < kMaxLatexPasses && LogWantsRerun(b)) {
        nPasses++;
        r = RunLatex(b);
        if (r != RunResult::Ok) {
            RememberFailure(b, r);
            return {};
        }
    }

    TempStr pdf = TexOutFileTemp(b, StrL(".pdf"));
    if (!file::Exists(pdf)) {
        RememberTexError(path, fmt("%s produced no PDF", b.program));
        return {};
    }
    RememberTexError(path, {});
    RememberBuild(path, root, pdf);
    return pdf;
}

// EngineTex compiles the source with a TeX distribution and proxies the
// resulting PDF through EngineMupdf, the way EnginePs does with Ghostscript
class EngineTex : public EngineBase {
  public:
    EngineTex() {
        kind = kindEngineTex;
        defaultExt = str::Dup(StrL(".tex"));
    }

    ~EngineTex() override {
        if (pdfEngine) {
            pdfEngine->Release();
        }
        str::Free(pdfPath);
    }

    EngineBase* Clone() override {
        EngineBase* newEngine = pdfEngine->Clone();
        if (!newEngine) {
            return {};
        }
        EngineTex* clone = new EngineTex();
        if (FilePath()) {
            clone->SetFilePath(FilePath());
        }
        clone->pdfEngine = newEngine;
        clone->pdfPath = str::Dup(pdfPath);
        return clone;
    }

    RectF PageMediabox(int pageNo) override { return pdfEngine->PageMediabox(pageNo); }

    RectF PageContentBox(int pageNo, RenderTarget target = RenderTarget::View) override {
        return pdfEngine->PageContentBox(pageNo, target);
    }

    Pixmap* RenderPage(RenderPageArgs& args) override { return pdfEngine->RenderPage(args); }

    RectF Transform(const RectF& rect, int pageNo, float zoom, int rotation, bool inverse = false) override {
        return pdfEngine->Transform(rect, pageNo, zoom, rotation, inverse);
    }

    Str GetFileData() override { return file::ReadFile(FilePath()); }

    bool SaveFileAs(Str dstPath) override {
        Str srcPath = FilePath();
        if (!srcPath) {
            return false;
        }
        return file::Copy(dstPath, srcPath, false);
    }

    PageText ExtractPageText(int pageNo) override { return pdfEngine->ExtractPageText(pageNo); }

    bool HasClipOptimizations(int pageNo) override { return pdfEngine->HasClipOptimizations(pageNo); }

    TempStr GetPropertyTemp(DocProp prop) override { return pdfEngine->GetPropertyTemp(prop); }

    bool BenchLoadPage(int pageNo) override { return pdfEngine->BenchLoadPage(pageNo); }

    Vec<IPageElement*> GetElements(int pageNo) override { return pdfEngine->GetElements(pageNo); }

    // don't delete the result
    IPageElement* GetElementAtPos(int pageNo, PointF pt) override { return pdfEngine->GetElementAtPos(pageNo, pt); }

    bool HandleLink(IPageDestination* dest, ILinkHandler* lh) override { return pdfEngine->HandleLink(dest, lh); }

    IPageDestination* GetNamedDest(Str name) override { return pdfEngine->GetNamedDest(name); }

    TocTree* GetToc() override { return pdfEngine->GetToc(); }

    EngineBase* pdfEngine = nullptr;
    Str pdfPath; // compiled output, next to the root .tex or in a temp dir

    bool Load(Str fileName) {
        pageCount = 0;
        ReportIf(FilePath() || pdfEngine);
        if (!fileName) {
            return false;
        }
        SetFilePath(fileName);

        TempStr pdf = CompileTexTemp(fileName);
        if (!pdf) {
            return false;
        }
        // loaded from memory so the next compile can overwrite the file
        Str pdfData = file::ReadFile(pdf);
        if (len(pdfData) == 0) {
            return false;
        }
        pdfEngine = CreateEngineMupdfFromData(pdfData, pdf, nullptr);
        str::Free(pdfData);
        if (!pdfEngine) {
            return false;
        }
        pdfPath = str::Dup(pdf);

        preferredLayout = pdfEngine->preferredLayout;
        fileDPI = pdfEngine->GetFileDPI();
        allowsPrinting = pdfEngine->AllowsPrinting();
        allowsCopyingText = pdfEngine->AllowsCopyingText();
        decryptionKey = str::Dup(arena, pdfEngine->decryptionKey);
        pageCount = pdfEngine->PageCount();

        return true;
    }
};

EngineBase* CreateEngineTexFromFile(Str fileName) {
    EngineTex* engine = new EngineTex();
    if (!engine->Load(fileName)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

bool IsEngineTexAvailable() {
    return len(TexBinDir()) > 0;
}

// why the last compile of path failed, {} if it succeeded or never ran
TempStr EngineTexLastErrorTemp(Str path) {
    gTexMutex.Lock();
    TempStr res;
    if (str::EqI(path, gTexErrorPath)) {
        res = str::DupTemp(gTexError);
    }
    gTexMutex.Unlock();
    return res;
}

// the compiled PDF, so SyncTeX can look up <root>.synctex next to it
Str EngineTexPdfPath(EngineBase* engine) {
    if (!engine || engine->kind != kindEngineTex) {
        return {};
    }
    return ((EngineTex*)engine)->pdfPath;
}
