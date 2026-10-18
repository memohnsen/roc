// Experimental harness: replicate Roc's LLVM optimize+emit (src/build/zig_llvm.cpp)
// on a dumped pre-optimization module, and try parallel variants.
//
// Usage: pcg <in.bc> <out-prefix> <mode> [threads]
//   mode = serial | split-cg | split-opt
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/MemCpyOptimizer.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/SplitModule.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>
#include <llvm/Transforms/Vectorize/SLPVectorizer.h>
#include <llvm/LTO/LTO.h>
#include <llvm/LTO/Config.h>
#include <llvm/Analysis/ModuleSummaryAnalysis.h>
#include <llvm/Support/Caching.h>
#include <llvm/Support/Threading.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <queue>
#include <thread>
#include <vector>

using namespace llvm;

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static int envInt(const char *name, int dflt) {
    const char *v = std::getenv(name);
    return v ? std::atoi(v) : dflt;
}

static std::unique_ptr<TargetMachine> makeTM(const Triple &triple) {
    std::string err;
    const Target *target = TargetRegistry::lookupTarget(triple, err);
    if (!target) { errs() << err << "\n"; exit(1); }
    TargetOptions opt;
    opt.UseInitArray = true;
    opt.FunctionSections = true;
    opt.DataSections = true;
    CodeGenOptLevel ol = CodeGenOptLevel::Aggressive;
    if (const char *l = std::getenv("PCG_CG_LEVEL")) ol = (CodeGenOptLevel)std::atoi(l);
    auto *tm = target->createTargetMachine(triple, "x86-64", "", opt, std::nullopt, std::nullopt, ol, false);
    tm->setFastISel(false);
    return std::unique_ptr<TargetMachine>(tm);
}

// Roc's middle end (zig_llvm.cpp emitWithBinStream, O3, no debug).
static void optimize(Module &m, TargetMachine &tm) {
    OptimizationLevel level = OptimizationLevel::O3;
    if (const char *l = std::getenv("PCG_OPT")) {
        if (!strcmp(l, "O2")) level = OptimizationLevel::O2;
        if (!strcmp(l, "O1")) level = OptimizationLevel::O1;
    }
    PipelineTuningOptions pto;
    pto.LoopUnrolling = true;
    pto.SLPVectorization = true;
    pto.LoopVectorization = false;
    pto.LoopInterleaving = true;
    pto.MergeFunctions = !envInt("PCG_NO_MERGEFUNC", 0);
    if (int t = envInt("PCG_INLINE_THRESHOLD", 0)) pto.InlinerThreshold = t;

    PassInstrumentationCallbacks pic;
    StandardInstrumentations si(m.getContext(), false);
    si.registerCallbacks(pic);
    PassBuilder pb(&tm, pto, std::nullopt, &pic);
    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    ModuleAnalysisManager mam;
    fam.registerPass([&] { return pb.buildDefaultAAPipeline(); });
    Triple triple(m.getTargetTriple());
    auto tlii = std::make_unique<TargetLibraryInfoImpl>(triple);
    tlii->disableAllFunctions();
    fam.registerPass([&] { return TargetLibraryAnalysis(*tlii); });
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    pb.registerPipelineStartEPCallback([&](ModulePassManager &mpm, OptimizationLevel) {
        FunctionPassManager fpm;
        fpm.addPass(SROAPass(SROAOptions::ModifyCFG));
        fpm.addPass(MemCpyOptPass());
        mpm.addPass(createModuleToFunctionPassAdaptor(std::move(fpm)));
    });
    ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(level);
    mpm.run(m, mam);

    if (!envInt("PCG_NO_REVEC", 0)) {
        FunctionPassManager rv;
        rv.addPass(LoopVectorizePass());
        rv.addPass(SLPVectorizerPass());
        rv.addPass(InstCombinePass());
        rv.addPass(SimplifyCFGPass());
        for (Function &f : m)
            if (!f.isDeclaration()) rv.run(f, fam);
    }
}


static PipelineTuningOptions rocPTO() {
    PipelineTuningOptions pto;
    pto.LoopUnrolling = true;
    pto.SLPVectorization = true;
    pto.LoopVectorization = false;
    pto.LoopInterleaving = true;
    pto.MergeFunctions = !envInt("PCG_NO_MERGEFUNC", 0);
    if (int t = envInt("PCG_INLINE_THRESHOLD", 0)) pto.InlinerThreshold = t;
    return pto;
}

// ThinLTO pre-link: Roc's copy forwarding plus the pre-link pipeline, then the
// module with its summary as bitcode.
static void prelink(Module &m, TargetMachine &tm, SmallVectorImpl<char> &out) {
    PipelineTuningOptions pto = rocPTO();
    PassInstrumentationCallbacks pic;
    StandardInstrumentations si(m.getContext(), false);
    si.registerCallbacks(pic);
    PassBuilder pb(&tm, pto, std::nullopt, &pic);
    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    ModuleAnalysisManager mam;
    fam.registerPass([&] { return pb.buildDefaultAAPipeline(); });
    Triple triple(m.getTargetTriple());
    auto tlii = std::make_unique<TargetLibraryInfoImpl>(triple);
    tlii->disableAllFunctions();
    fam.registerPass([&] { return TargetLibraryAnalysis(*tlii); });
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    pb.registerPipelineStartEPCallback([&](ModulePassManager &mpm, OptimizationLevel) {
        FunctionPassManager fpm;
        fpm.addPass(SROAPass(SROAOptions::ModifyCFG));
        fpm.addPass(MemCpyOptPass());
        mpm.addPass(createModuleToFunctionPassAdaptor(std::move(fpm)));
    });
    ModulePassManager mpm = pb.buildThinLTOPreLinkDefaultPipeline(OptimizationLevel::O3);
    mpm.run(m, mam);
    auto &index = mam.getResult<ModuleSummaryIndexAnalysis>(m);
    raw_svector_ostream os(out);
    WriteBitcodeToFile(m, os, false, &index);
}

static void codegen(Module &m, TargetMachine &tm, SmallVectorImpl<char> &out) {
    raw_svector_ostream os(out);
    legacy::PassManager cg;
    cg.add(createTargetTransformInfoWrapperPass(tm.getTargetIRAnalysis()));
    if (tm.addPassesToEmitFile(cg, os, nullptr, CodeGenFileType::ObjectFile)) {
        errs() << "cannot emit\n";
        exit(1);
    }
    cg.run(m);
}

static void writeFile(const std::string &path, ArrayRef<char> bytes) {
    std::error_code ec;
    raw_fd_ostream os(path, ec);
    os.write(bytes.data(), bytes.size());
}

static std::unique_ptr<Module> parseIn(MemoryBufferRef buf, LLVMContext &ctx) {
    auto mod = parseBitcodeFile(buf, ctx);
    if (!mod) { errs() << "parse failed: " << toString(mod.takeError()) << "\n"; exit(1); }
    return std::move(*mod);
}

static unsigned fnSize(const Function &f) { return f.getInstructionCount(); }

// ---- split-opt partitioner ----
// Partitions function definitions so that each local function with exactly one
// caller stays with that caller; small local functions referenced across
// partitions are copied into each user as available_externally (imported).
struct Plan {
    unsigned n;
    DenseMap<const GlobalValue *, unsigned> owner;   // definition owner
    std::vector<DenseSet<const GlobalValue *>> imports; // per partition, available_externally copies
};

static void collectRefs(const User *u, SmallVectorImpl<const GlobalValue *> &out, DenseSet<const User *> &seen) {
    for (const Value *op : u->operands()) {
        if (auto *gv = dyn_cast<GlobalValue>(op)) {
            out.push_back(gv);
        } else if (auto *c = dyn_cast<Constant>(op)) {
            if (seen.insert(c).second) collectRefs(c, out, seen);
        }
    }
}

static Plan planSplit(Module &m, unsigned n) {
    Plan plan;
    plan.n = n;
    plan.imports.resize(n);
    const unsigned import_limit = envInt("PCG_IMPORT_LIMIT", 0);
    const unsigned import_depth = envInt("PCG_IMPORT_DEPTH", 1);
    const bool import_consts = envInt("PCG_IMPORT_CONST", 1);

    // Definitions and their references.
    std::vector<const GlobalValue *> defs;
    DenseMap<const GlobalValue *, unsigned> idx;
    for (GlobalValue &gv : m.global_values()) {
        if (gv.isDeclaration()) continue;
        idx[&gv] = defs.size();
        defs.push_back(&gv);
    }
    std::vector<SmallVector<const GlobalValue *, 8>> refs(defs.size());
    std::vector<SmallVector<unsigned, 4>> users(defs.size()); // distinct referencing defs
    for (unsigned i = 0; i < defs.size(); i++) {
        DenseSet<const User *> seen;
        SmallVector<const GlobalValue *, 32> raw;
        if (auto *f = dyn_cast<Function>(defs[i])) {
            for (const Instruction &inst : instructions(*f)) collectRefs(&inst, raw, seen);
        } else if (auto *gvar = dyn_cast<GlobalVariable>(defs[i])) {
            if (gvar->hasInitializer()) collectRefs(gvar, raw, seen);
        } else if (auto *ga = dyn_cast<GlobalAlias>(defs[i])) {
            collectRefs(ga, raw, seen);
        }
        DenseSet<const GlobalValue *> uniq;
        for (auto *r : raw)
            if (!r->isDeclaration() && r != defs[i] && uniq.insert(r).second) {
                refs[i].push_back(r);
                users[idx[r]].push_back(i);
            }
    }

    // Union-find clusters.
    std::vector<unsigned> parent(defs.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<unsigned(unsigned)> find = [&](unsigned x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](unsigned a, unsigned b) { parent[find(a)] = find(b); };

    std::vector<unsigned> single_edges;
    for (unsigned i = 0; i < defs.size(); i++) {
        const GlobalValue *gv = defs[i];
        bool local = gv->hasLocalLinkage();
        // Aliases and their aliasees, comdats stay together.
        if (isa<GlobalAlias>(gv)) { for (auto *r : refs[i]) unite(i, idx[r]); continue; }
        if (const Comdat *c = gv->getComdat()) (void)c; // Roc emits none
        if (isa<GlobalVariable>(gv)) {
            // Variables join their single user; otherwise stay alone.
            if (users[i].size() == 1) unite(i, users[i][0]);
            continue;
        }
        if (local && users[i].size() == 1 && isa<Function>(defs[users[i][0]])) single_edges.push_back(i);
    }
    {
        std::vector<uint64_t> own(defs.size(), 1);
        uint64_t total = 0;
        for (unsigned i = 0; i < defs.size(); i++) {
            if (auto *f = dyn_cast<Function>(defs[i])) own[i] += fnSize(*f);
            total += own[i];
        }
        std::vector<uint64_t> csz(defs.size(), 0);
        for (unsigned i = 0; i < defs.size(); i++) csz[find(i)] += own[i];
        const unsigned cap_div = envInt("PCG_CAP_DIV", 2);
        const uint64_t cap = cap_div ? total / n / cap_div : UINT64_MAX;
        std::sort(single_edges.begin(), single_edges.end(), [&](unsigned a, unsigned b) { return own[a] < own[b]; });
        unsigned skipped = 0;
        for (unsigned i : single_edges) {
            unsigned a = find(i), b = find(users[i][0]);
            if (a == b) continue;
            if (csz[a] + csz[b] > cap) { skipped++; continue; }
            parent[a] = b;
            csz[b] += csz[a];
        }
        errs() << "cap=" << cap << " single-caller edges=" << single_edges.size() << " skipped=" << skipped << "\n";
    }

    // Cluster sizes and LPT assignment.
    DenseMap<unsigned, uint64_t> csize;
    for (unsigned i = 0; i < defs.size(); i++) {
        uint64_t s = 1;
        if (auto *f = dyn_cast<Function>(defs[i])) s += fnSize(*f);
        csize[find(i)] += s;
    }
    std::vector<std::pair<uint64_t, unsigned>> clusters;
    for (auto &kv : csize) clusters.push_back({kv.second, kv.first});
    std::sort(clusters.begin(), clusters.end(), std::greater<>());
    using PQ = std::pair<uint64_t, unsigned>;
    std::priority_queue<PQ, std::vector<PQ>, std::greater<PQ>> pq;
    for (unsigned p = 0; p < n; p++) pq.push({0, p});
    DenseMap<unsigned, unsigned> cpart;
    std::vector<uint64_t> psize(n, 0);
    for (auto &c : clusters) {
        auto [s, p] = pq.top();
        pq.pop();
        cpart[c.second] = p;
        psize[p] = s + c.first;
        pq.push({s + c.first, p});
    }
    for (unsigned i = 0; i < defs.size(); i++) plan.owner[defs[i]] = cpart[find(i)];
    errs() << "clusters=" << clusters.size() << " biggest=" << clusters[0].first << " partitions:";
    for (auto s : psize) errs() << " " << s;
    errs() << "\n";

    // Imports: small functions referenced from another partition.
    if (import_limit || envInt("PCG_IMPORT_ALWAYS", 0)) {
        for (unsigned p = 0; p < n; p++) {
            SmallVector<std::pair<const GlobalValue *, unsigned>, 64> work;
            for (unsigned i = 0; i < defs.size(); i++)
                if (plan.owner[defs[i]] == p)
                    for (auto *r : refs[i]) work.push_back({r, 1});
            while (!work.empty()) {
                auto [r, depth] = work.pop_back_val();
                if (plan.owner[r] == p) continue;
                if (auto *gvar = dyn_cast<GlobalVariable>(r)) {
                    if (import_consts && gvar->isConstant() && gvar->hasInitializer() && !gvar->isInterposable())
                        if (plan.imports[p].insert(r).second)
                            for (auto *rr : refs[idx[r]]) work.push_back({rr, depth});
                    continue;
                }
                auto *f = dyn_cast<Function>(r);
                if (!f) continue;
                bool always = f->hasFnAttribute(Attribute::AlwaysInline);
                if (!always && fnSize(*f) > import_limit) continue;
                if (!plan.imports[p].insert(r).second) continue;
                if (depth < import_depth)
                    for (auto *rr : refs[idx[r]]) work.push_back({rr, depth + 1});
            }
        }
        size_t total = 0;
        for (auto &s : plan.imports) total += s.size();
        errs() << "imports total=" << total << "\n";
    }
    return plan;
}

// Clone partition p of m (owner definitions; imports as available_externally; the rest
// as declarations) and serialize it.
static void emitPartition(Module &m, const Plan &plan, unsigned p,
                          const DenseMap<const GlobalValue *, bool> &crossRef,
                          SmallVectorImpl<char> &out) {
    ValueToValueMapTy vmap;
    auto part = CloneModule(m, vmap, [&](const GlobalValue *gv) {
        auto it = plan.owner.find(gv);
        if (it != plan.owner.end() && it->second == p) return true;
        return plan.imports[p].count(gv) > 0;
    });
    for (GlobalValue &gv : part->global_values()) {
        if (gv.isDeclaration()) continue;
        // Find the original.
        const GlobalValue *orig = m.getNamedValue(gv.getName());
        if (!orig) continue;
        auto it = plan.owner.find(orig);
        bool owned = it != plan.owner.end() && it->second == p;
        if (!owned) {
            // imported copy
            gv.setLinkage(GlobalValue::AvailableExternallyLinkage);
            continue;
        }
    }
    raw_svector_ostream os(out);
    WriteBitcodeToFile(*part, os);
}

int main(int argc, char **argv) {
    if (argc < 4) { errs() << "usage\n"; return 1; }
    InitializeAllTargetInfos();
    InitializeAllTargets();
    InitializeAllTargetMCs();
    InitializeAllAsmPrinters();
    std::string in = argv[1], outp = argv[2], mode = argv[3];
    unsigned threads = argc > 4 ? std::atoi(argv[4]) : 6;
    if (const char *extra = std::getenv("PCG_LLVM_ARGS")) {
        SmallVector<const char *, 8> args{"pcg"};
        std::string s = extra;
        static std::vector<std::string> keep;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t e = s.find(' ', pos);
            if (e == std::string::npos) e = s.size();
            if (e > pos) keep.push_back(s.substr(pos, e - pos));
            pos = e + 1;
        }
        for (auto &k : keep) args.push_back(k.c_str());
        cl::ParseCommandLineOptions(args.size(), args.data());
    }

    double t0 = now_s();
    auto inbuf = MemoryBuffer::getFile(in);
    if (!inbuf) { errs() << "read failed\n"; return 1; }
    LLVMContext ctx;
    auto m = parseIn((*inbuf)->getMemBufferRef(), ctx);
    Triple triple(m->getTargetTriple());
    double t1 = now_s();
    errs() << "parse " << (t1 - t0) << " s, functions=" << m->size() << "\n";

    if (mode == "serial") {
        auto tm = makeTM(triple);
        optimize(*m, *tm);
        double t2 = now_s();
        errs() << "opt " << (t2 - t1) << " s\n";
        SmallVector<char, 0> obj;
        codegen(*m, *tm, obj);
        double t3 = now_s();
        errs() << "codegen " << (t3 - t2) << " s\n";
        writeFile(outp + ".0.o", obj);
        errs() << "TOTAL " << (t3 - t0) << " s\n";
        return 0;
    }

    if (mode == "split-cg") {
        auto tm = makeTM(triple);
        optimize(*m, *tm);
        double t2 = now_s();
        errs() << "opt " << (t2 - t1) << " s\n";
        std::vector<SmallVector<char, 0>> bcs;
        SplitModule(*m, threads, [&](std::unique_ptr<Module> part) {
            bcs.emplace_back();
            raw_svector_ostream os(bcs.back());
            WriteBitcodeToFile(*part, os);
        }, /*PreserveLocals=*/false, /*RoundRobin=*/false);
        m.reset();
        double t3 = now_s();
        errs() << "split " << (t3 - t2) << " s\n";
        std::vector<std::thread> ts;
        for (unsigned i = 0; i < bcs.size(); i++) {
            ts.emplace_back([&, i] {
                double s = now_s();
                LLVMContext c;
                auto pm = parseIn(MemoryBufferRef(StringRef(bcs[i].data(), bcs[i].size()), "part"), c);
                auto ttm = makeTM(triple);
                SmallVector<char, 0> obj;
                codegen(*pm, *ttm, obj);
                writeFile(outp + "." + std::to_string(i) + ".o", obj);
                errs() << "  part " << i << " codegen " << (now_s() - s) << " s\n";
            });
        }
        for (auto &t : ts) t.join();
        double t4 = now_s();
        errs() << "codegen " << (t4 - t3) << " s\nTOTAL " << (t4 - t0) << " s\n";
        return 0;
    }

    if (mode == "split-opt") {
        // Externalize locals referenced across partitions; keep the rest internal.
        Plan plan = planSplit(*m, threads);
        // Which locals are referenced from another partition (or imported)?
        DenseMap<const GlobalValue *, bool> crossRef;
        for (Function &f : *m) {
            if (f.isDeclaration()) continue;
            unsigned p = plan.owner[&f];
            DenseSet<const User *> seen;
            SmallVector<const GlobalValue *, 32> raw;
            for (const Instruction &inst : instructions(f)) collectRefs(&inst, raw, seen);
            for (auto *r : raw)
                if (!r->isDeclaration() && plan.owner[r] != p) crossRef[r] = true;
        }
        for (GlobalVariable &g : m->globals()) {
            if (g.isDeclaration() || !g.hasInitializer()) continue;
            unsigned p = plan.owner[&g];
            DenseSet<const User *> seen;
            SmallVector<const GlobalValue *, 32> raw;
            collectRefs(&g, raw, seen);
            for (auto *r : raw)
                if (!r->isDeclaration() && plan.owner[r] != p) crossRef[r] = true;
        }
        for (unsigned p = 0; p < threads; p++)
            for (auto *gv : plan.imports[p]) {
                crossRef[gv] = true;
                DenseSet<const User *> seen;
                SmallVector<const GlobalValue *, 32> raw;
                if (auto *f = dyn_cast<Function>(gv)) for (const Instruction &inst : instructions(*f)) collectRefs(&inst, raw, seen);
                else collectRefs(gv, raw, seen);
                for (auto *r : raw)
                    if (!r->isDeclaration()) crossRef[r] = true;
            }
        unsigned externalized = 0;
        for (GlobalValue &gv : m->global_values()) {
            if (gv.isDeclaration() || !gv.hasLocalLinkage()) continue;
            if (!crossRef.lookup(&gv)) continue;
            if (!gv.hasName()) gv.setName("__pcg_unnamed");
            gv.setLinkage(GlobalValue::ExternalLinkage);
            gv.setVisibility(GlobalValue::HiddenVisibility);
            externalized++;
        }
        errs() << "externalized=" << externalized << "\n";
        std::vector<SmallVector<char, 0>> bcs(threads);
        for (unsigned p = 0; p < threads; p++) emitPartition(*m, plan, p, crossRef, bcs[p]);
        m.reset();
        double t2 = now_s();
        errs() << "split " << (t2 - t1) << " s\n";
        std::vector<std::thread> ts;
        unsigned workers = envInt("PCG_THREADS", threads);
        // Largest partitions first.
        std::vector<unsigned> order(threads);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) { return bcs[a].size() > bcs[b].size(); });
        std::atomic<unsigned> next{0};
        for (unsigned w = 0; w < workers; w++) {
            ts.emplace_back([&] {
              for (unsigned k; (k = next.fetch_add(1)) < threads;) {
                unsigned i = order[k];
                double s = now_s();
                LLVMContext c;
                auto pm = parseIn(MemoryBufferRef(StringRef(bcs[i].data(), bcs[i].size()), "part"), c);
                auto ttm = makeTM(triple);
                optimize(*pm, *ttm);
                double so = now_s();
                SmallVector<char, 0> obj;
                codegen(*pm, *ttm, obj);
                writeFile(outp + "." + std::to_string(i) + ".o", obj);
                errs() << "  part " << i << " opt " << (so - s) << " s codegen " << (now_s() - so) << " s\n";
              }
            });
        }
        for (auto &t : ts) t.join();
        double t3 = now_s();
        errs() << "opt+codegen " << (t3 - t2) << " s\nTOTAL " << (t3 - t0) << " s\n";
        return 0;
    }

    if (mode == "thin") {
        Plan plan = planSplit(*m, threads);
        DenseSet<StringRef> exportedNames;
        std::vector<std::string> exportedStore;
        for (GlobalValue &gv : m->global_values())
            if (!gv.isDeclaration() && !gv.hasLocalLinkage()) exportedStore.push_back(gv.getName().str());
        DenseMap<const GlobalValue *, bool> crossRef;
        auto markCross = [&](const GlobalValue *def, unsigned p) {
            DenseSet<const User *> seen;
            SmallVector<const GlobalValue *, 32> raw;
            if (auto *f = dyn_cast<Function>(def)) for (const Instruction &inst : instructions(*f)) collectRefs(&inst, raw, seen);
            else collectRefs(def, raw, seen);
            for (auto *r : raw)
                if (!r->isDeclaration() && plan.owner[r] != p) crossRef[r] = true;
        };
        for (GlobalValue &gv : m->global_values())
            if (!gv.isDeclaration()) markCross(&gv, plan.owner[&gv]);
        unsigned externalized = 0;
        for (GlobalValue &gv : m->global_values()) {
            if (gv.isDeclaration() || !gv.hasLocalLinkage() || !crossRef.lookup(&gv)) continue;
            if (!gv.hasName()) gv.setName("__pcg_unnamed");
            gv.setLinkage(GlobalValue::ExternalLinkage);
            gv.setVisibility(GlobalValue::HiddenVisibility);
            externalized++;
        }
        errs() << "externalized=" << externalized << "\n";
        std::vector<SmallVector<char, 0>> bcs(threads);
        for (unsigned p = 0; p < threads; p++) emitPartition(*m, plan, p, crossRef, bcs[p]);
        m.reset();
        double t2 = now_s();
        errs() << "split " << (t2 - t1) << " s\n";
        std::vector<SmallVector<char, 0>> thin(threads);
        {
            std::vector<std::thread> ts;
            for (unsigned i = 0; i < threads; i++) {
                ts.emplace_back([&, i] {
                    double s = now_s();
                    LLVMContext c;
                    auto pm = parseIn(MemoryBufferRef(StringRef(bcs[i].data(), bcs[i].size()), "part"), c);
                    auto ttm = makeTM(triple);
                    prelink(*pm, *ttm, thin[i]);
                    errs() << "  part " << i << " prelink " << (now_s() - s) << " s\n";
                });
            }
            for (auto &t : ts) t.join();
        }
        bcs.clear();
        double t3 = now_s();
        errs() << "prelink " << (t3 - t2) << " s\n";

        StringSet<> exported;
        for (auto &n : exportedStore) exported.insert(n);
        lto::Config conf;
        conf.CPU = "x86-64";
        conf.Options.UseInitArray = true;
        conf.Options.FunctionSections = true;
        conf.Options.DataSections = true;
        conf.CGOptLevel = CodeGenOptLevel::Aggressive;
        conf.OptLevel = 3;
        conf.PTO = rocPTO();
        conf.Freestanding = true;
        conf.DefaultTriple = triple.str();
        if (!envInt("PCG_NO_REVEC", 0))
            conf.OptPipeline = "thinlto<O3>,function(loop-vectorize,slp-vectorizer,instcombine,simplifycfg)";
        if (const char *pl = std::getenv("PCG_THIN_PIPELINE")) conf.OptPipeline = pl;
        lto::LTO lto(std::move(conf), lto::createInProcessThinBackend(heavyweight_hardware_concurrency(threads)));
        std::vector<std::string> names(threads);
        for (unsigned i = 0; i < threads; i++) {
            names[i] = "part" + std::to_string(i);
            auto file = lto::InputFile::create(MemoryBufferRef(StringRef(thin[i].data(), thin[i].size()), names[i]));
            if (!file) { errs() << toString(file.takeError()) << "\n"; return 1; }
            std::vector<lto::SymbolResolution> res;
            for (const lto::InputFile::Symbol &sym : (*file)->symbols()) {
                lto::SymbolResolution r;
                bool defined = !sym.isUndefined();
                r.Prevailing = defined;
                r.FinalDefinitionInLinkageUnit = defined;
                r.VisibleToRegularObj = defined && exported.count(sym.getName());
                res.push_back(r);
            }
            if (auto err = lto.add(std::move(*file), res)) { errs() << toString(std::move(err)) << "\n"; return 1; }
        }
        std::vector<SmallVector<char, 0>> objs(threads + 2);
        std::mutex mu;
        auto addStream = [&](unsigned task, const Twine &) -> Expected<std::unique_ptr<CachedFileStream>> {
            return std::make_unique<CachedFileStream>(std::make_unique<raw_svector_ostream>(objs[task]));
        };
        if (auto err = lto.run(addStream)) { errs() << toString(std::move(err)) << "\n"; return 1; }
        double t4 = now_s();
        for (unsigned i = 0; i < objs.size(); i++)
            if (!objs[i].empty()) writeFile(outp + "." + std::to_string(i) + ".o", objs[i]);
        errs() << "thin backend " << (t4 - t3) << " s\nTOTAL " << (t4 - t0) << " s\n";
        return 0;
    }
    errs() << "unknown mode\n";
    return 1;
}
