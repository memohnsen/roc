// Adapted from the Zig compiler at https://codeberg.org/ziglang/zig and licensed under the MIT license. Thanks, Zig team!


/*
 * The point of this file is to contain all the LLVM C++ API interaction so that:
 * 1. The compile time of other files is kept under control.
 * 2. Provide a C interface to the LLVM functions we need for self-hosting purposes.
 * 3. Prevent C++ from infecting the rest of the project.
 */

#include "zig_llvm.h"

#if __GNUC__ >= 9
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winit-list-lifetime"
#endif

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/OptBisect.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/InitializePasses.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>
#include <llvm/Transforms/Vectorize/SLPVectorizer.h>
#include <llvm/Transforms/Scalar/MemCpyOptimizer.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/COFFImportFile.h>
#include <llvm/Object/COFFModuleDefinition.h>
#include <llvm/PassRegistry.h>
#include <llvm/BinaryFormat/Magic.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Support/Timer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Target/CodeGenCWrappers.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Instrumentation/ThreadSanitizer.h>
#include <llvm/Transforms/Instrumentation/SanitizerCoverage.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Transforms/Utils/AddDiscriminators.h>
#include <llvm/Transforms/Utils/CanonicalizeAliases.h>
#include <llvm/Transforms/Utils/LowerMemIntrinsics.h>
#include <llvm/Transforms/Utils/NameAnonGlobals.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/thread.h>

#include <lld/Common/CommonLinkerContext.h>
#include <lld/Common/Driver.h>

#if __GNUC__ >= 9
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>
#include <numeric>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include <stdlib.h>

using namespace llvm;

#ifndef NDEBUG
static const bool assertions_on = true;
#else
static const bool assertions_on = false;
#endif

static OptimizationLevel toLLVMOptimizationLevel(ZigLLVMIROptimizationLevel level) {
    switch (level) {
        case ZigLLVMIROptimizationLevel_Oz:
            return OptimizationLevel::Oz;
        case ZigLLVMIROptimizationLevel_O3:
            return OptimizationLevel::O3;
        case ZigLLVMIROptimizationLevel_O0:
            return OptimizationLevel::O0;
    }

    llvm_unreachable("invalid LLVM IR optimization level");
}

namespace {
struct LowerMemoryIntrinsicsPass : PassInfoMixin<LowerMemoryIntrinsicsPass> {
    TargetIRAnalysis target_ir_analysis;

    explicit LowerMemoryIntrinsicsPass(TargetIRAnalysis target_ir_analysis)
        : target_ir_analysis(std::move(target_ir_analysis)) {}

    PreservedAnalyses run(Function &function, FunctionAnalysisManager &function_am) {
        SmallVector<Instruction *, 16> intrinsics;

        for (BasicBlock &block : function) {
            for (Instruction &instruction : block) {
                if (isa<MemCpyInst>(&instruction) ||
                    isa<MemMoveInst>(&instruction) ||
                    isa<MemSetInst>(&instruction) ||
                    isa<MemSetPatternInst>(&instruction)) {
                    intrinsics.push_back(&instruction);
                }
            }
        }

        if (intrinsics.empty()) {
            return PreservedAnalyses::all();
        }

        TargetTransformInfo tti = target_ir_analysis.run(function, function_am);
        bool changed = false;

        for (Instruction *instruction : intrinsics) {
            if (auto *memcpy = dyn_cast<MemCpyInst>(instruction)) {
                expandMemCpyAsLoop(memcpy, tti);
                instruction->eraseFromParent();
                changed = true;
            } else if (auto *memmove = dyn_cast<MemMoveInst>(instruction)) {
                if (expandMemMoveAsLoop(memmove, tti)) {
                    instruction->eraseFromParent();
                    changed = true;
                }
            } else if (auto *memset = dyn_cast<MemSetInst>(instruction)) {
                expandMemSetAsLoop(memset);
                instruction->eraseFromParent();
                changed = true;
            } else if (auto *memset_pattern = dyn_cast<MemSetPatternInst>(instruction)) {
                expandMemSetPatternAsLoop(memset_pattern);
                instruction->eraseFromParent();
                changed = true;
            }
        }

        return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
};
} // end anonymous namespace

LLVMTargetMachineRef ZigLLVMCreateTargetMachine(LLVMTargetRef T, const char *Triple,
    const char *CPU, const char *Features, LLVMCodeGenOptLevel Level, LLVMRelocMode Reloc,
    LLVMCodeModel CodeModel, bool function_sections, bool data_sections, ZigLLVMFloatABI float_abi,
    const char *abi_name, bool emulated_tls)
{
    std::optional<Reloc::Model> RM;
    switch (Reloc){
        case LLVMRelocStatic:
            RM = Reloc::Static;
            break;
        case LLVMRelocPIC:
            RM = Reloc::PIC_;
            break;
        case LLVMRelocDynamicNoPic:
            RM = Reloc::DynamicNoPIC;
            break;
        case LLVMRelocROPI:
            RM = Reloc::ROPI;
            break;
        case LLVMRelocRWPI:
            RM = Reloc::RWPI;
            break;
        case LLVMRelocROPI_RWPI:
            RM = Reloc::ROPI_RWPI;
            break;
        default:
            break;
    }

    bool JIT;
    std::optional<CodeModel::Model> CM = unwrap(CodeModel, JIT);

    CodeGenOptLevel OL;
    switch (Level) {
        case LLVMCodeGenLevelNone:
            OL = CodeGenOptLevel::None;
            break;
        case LLVMCodeGenLevelLess:
            OL = CodeGenOptLevel::Less;
            break;
        case LLVMCodeGenLevelAggressive:
            OL = CodeGenOptLevel::Aggressive;
            break;
        default:
            OL = CodeGenOptLevel::Default;
            break;
    }

    TargetOptions opt;

    opt.UseInitArray = true;
    opt.FunctionSections = function_sections;
    opt.DataSections = data_sections;
    switch (float_abi) {
        case ZigLLVMFloatABI_Default:
            opt.FloatABIType = FloatABI::Default;
            break;
        case ZigLLVMFloatABI_Soft:
            opt.FloatABIType = FloatABI::Soft;
            break;
        case ZigLLVMFloatABI_Hard:
            opt.FloatABIType = FloatABI::Hard;
            break;
    }

    if (abi_name != nullptr) {
        opt.MCOptions.ABIName = abi_name;
    }

    if (emulated_tls) {
        opt.EmulatedTLS = true;
    }

    TargetMachine *TM = reinterpret_cast<Target*>(T)->createTargetMachine(llvm::Triple(Triple), CPU, Features, opt, RM, CM,
            OL, JIT);
    return reinterpret_cast<LLVMTargetMachineRef>(TM);
}

namespace {
// LLVM's time profiler can provide a hierarchy view of the time spent
// in each component. It generates JSON report in Chrome's "Trace Event"
// format. So the report can be easily visualized by the Chrome browser.
struct TimeTracerRAII {
  // Granularity in ms
  unsigned TimeTraceGranularity;
  StringRef TimeTraceFile, OutputFilename;
  bool EnableTimeTrace;

  TimeTracerRAII(StringRef ProgramName, StringRef OF)
    : TimeTraceGranularity(500U),
      TimeTraceFile(std::getenv("ZIG_LLVM_TIME_TRACE_FILE")),
      OutputFilename(OF),
      EnableTimeTrace(!TimeTraceFile.empty()) {
    if (EnableTimeTrace) {
      if (const char *G = std::getenv("ZIG_LLVM_TIME_TRACE_GRANULARITY"))
        TimeTraceGranularity = (unsigned)std::atoi(G);

      llvm::timeTraceProfilerInitialize(TimeTraceGranularity, ProgramName);
    }
  }

  ~TimeTracerRAII() {
    if (EnableTimeTrace) {
      if (auto E = llvm::timeTraceProfilerWrite(TimeTraceFile, OutputFilename)) {
        handleAllErrors(std::move(E), [&](const StringError &SE) {
          errs() << SE.getMessage() << "\n";
        });
        return;
      }
      timeTraceProfilerCleanup();
    }
  }
};
} // end anonymous namespace

static SanitizerCoverageOptions getSanCovOptions(ZigLLVMCoverageOptions z) {
    SanitizerCoverageOptions o;
    o.CoverageType = (SanitizerCoverageOptions::Type)z.CoverageType;
    o.IndirectCalls = z.IndirectCalls;
    o.TraceBB = z.TraceBB;
    o.TraceCmp = z.TraceCmp;
    o.TraceDiv = z.TraceDiv;
    o.TraceGep = z.TraceGep;
    o.Use8bitCounters = z.Use8bitCounters;
    o.TracePC = z.TracePC;
    o.TracePCGuard = z.TracePCGuard;
    o.Inline8bitCounters = z.Inline8bitCounters;
    o.InlineBoolFlag = z.InlineBoolFlag;
    o.PCTable = z.PCTable;
    o.NoPrune = z.NoPrune;
    o.StackDepth = z.StackDepth;
    o.TraceLoads = z.TraceLoads;
    o.TraceStores = z.TraceStores;
    o.CollectControlFlow = z.CollectControlFlow;
    return o;
}

ZIG_EXTERN_C void ZigLLVMRunGlobalDCE(LLVMModuleRef module_ref) {
    Module &llvm_module = *unwrap(module_ref);

    PipelineTuningOptions pipeline_opts;
    PassBuilder pass_builder(nullptr, pipeline_opts, std::nullopt, nullptr);
    ModuleAnalysisManager module_am;
    pass_builder.registerModuleAnalyses(module_am);

    ModulePassManager module_pm;
    module_pm.addPass(GlobalDCEPass());
    module_pm.run(llvm_module, module_am);
}

static bool openEmitFile(const char *filename, std::unique_ptr<raw_fd_ostream> &dest, char **error_message) {
    if (!filename) return false;
    std::error_code EC;
    dest.reset(new(std::nothrow) raw_fd_ostream(filename, EC, sys::fs::OF_None));
    if (EC) {
        *error_message = strdup((const char *)StringRef(EC.message()).bytes_begin());
        return true;
    }
    return false;
}

// Runs the optimization and code generation pipeline, writing the object (or
// LTO bitcode) to `dest_bin` when it is non-null. The assembly and bitcode
// outputs named in `options` are written to their files.
static bool emitWithBinStream(LLVMTargetMachineRef targ_machine_ref, LLVMModuleRef module_ref,
    char **error_message, const ZigLLVMEmitOptions *options, raw_pwrite_stream *dest_bin,
    const char *time_trace_name)
{
    std::unique_ptr<raw_fd_ostream> dest_asm, dest_bitcode;
    if (openEmitFile(options->asm_filename, dest_asm, error_message)) return true;
    if (openEmitFile(options->bitcode_filename, dest_bitcode, error_message)) return true;

    auto PID = sys::Process::getProcessId();
    std::string ProcName = "zig-";
    ProcName += std::to_string(PID);
    TimeTracerRAII TimeTracer(ProcName, time_trace_name);

    TargetMachine &target_machine = *reinterpret_cast<TargetMachine*>(targ_machine_ref);

    if (options->allow_fast_isel) {
        target_machine.setO0WantsFastISel(true);
    } else {
        target_machine.setFastISel(false);
    }

    if (!options->allow_machine_outliner) {
        target_machine.setMachineOutliner(false);
    }

    Module &llvm_module = *unwrap(module_ref);

    // Pipeline configurations
    const OptimizationLevel opt_level = toLLVMOptimizationLevel(options->ir_opt_level);
    PipelineTuningOptions pipeline_opts;
    pipeline_opts.LoopUnrolling = !options->is_debug;
    pipeline_opts.SLPVectorization = !options->is_debug;
    pipeline_opts.LoopVectorization = false; // https://github.com/llvm/llvm-project/issues/186922
    pipeline_opts.LoopInterleaving = !options->is_debug;
    pipeline_opts.MergeFunctions = !options->is_debug;

    // Instrumentations
    PassInstrumentationCallbacks instr_callbacks;
    StandardInstrumentations std_instrumentations(llvm_module.getContext(), false);
    std_instrumentations.registerCallbacks(instr_callbacks);

    std::optional<PGOOptions> opt_pgo_options = {};
    PassBuilder pass_builder(&target_machine, pipeline_opts,
                             opt_pgo_options, &instr_callbacks);

    LoopAnalysisManager loop_am;
    FunctionAnalysisManager function_am;
    CGSCCAnalysisManager cgscc_am;
    ModuleAnalysisManager module_am;

    // Register the AA manager first so that our version is the one used
    function_am.registerPass([&] {
      return pass_builder.buildDefaultAAPipeline();
    });

    Triple target_triple(llvm_module.getTargetTriple());
    auto tlii = std::make_unique<TargetLibraryInfoImpl>(target_triple);
    if (options->no_target_libcalls) {
        tlii->disableAllFunctions();
    }
    function_am.registerPass([&] { return TargetLibraryAnalysis(*tlii); });

    // Initialize the AnalysisManagers
    pass_builder.registerModuleAnalyses(module_am);
    pass_builder.registerCGSCCAnalyses(cgscc_am);
    pass_builder.registerFunctionAnalyses(function_am);
    pass_builder.registerLoopAnalyses(loop_am);
    pass_builder.crossRegisterProxies(loop_am, function_am, cgscc_am, module_am);

    pass_builder.registerPipelineStartEPCallback([&](ModulePassManager &module_pm, OptimizationLevel level) {
        // Verify the input
        if (assertions_on) {
            module_pm.addPass(VerifierPass());
        }

        module_pm.addPass(createModuleToFunctionPassAdaptor(AddDiscriminatorsPass()));

        // Roc's code generator gives every value a stack slot and moves values
        // between slots with memcpy, so a large procedure can copy one slot (a
        // big record parameter, say) into thousands of others. SROA, which the
        // default pipeline runs before MemCpyOpt, rescans a slot's uses every
        // time it rewrites one of the slots copied from it, which is quadratic
        // in the number of copies: one 300,000-line procedure spent 160 s in a
        // single SROA run. Forwarding the copy chains first leaves SROA linear
        // work; the same procedure then optimizes in about 36 s in total. Both
        // passes run again later in the default pipeline, so this only moves
        // work earlier and does not change which transformations are allowed.
        if (level != OptimizationLevel::O0) {
            FunctionPassManager copy_forwarding;
            copy_forwarding.addPass(SROAPass(SROAOptions::ModifyCFG));
            copy_forwarding.addPass(MemCpyOptPass());
            module_pm.addPass(createModuleToFunctionPassAdaptor(std::move(copy_forwarding)));
        }
    });

    const bool early_san = options->is_debug;

    pass_builder.registerOptimizerEarlyEPCallback([&](ModulePassManager &module_pm, OptimizationLevel level, ThinOrFullLTOPhase lto_phase) {
        if (early_san) {
            // Code coverage instrumentation.
            if (options->sancov) {
                module_pm.addPass(SanitizerCoveragePass(getSanCovOptions(options->coverage)));
            }

            // Thread sanitizer
            if (options->tsan) {
                module_pm.addPass(ModuleThreadSanitizerPass());
                module_pm.addPass(createModuleToFunctionPassAdaptor(ThreadSanitizerPass()));
            }
        }
    });

    pass_builder.registerOptimizerLastEPCallback([&](ModulePassManager &module_pm, OptimizationLevel level, ThinOrFullLTOPhase lto_phase) {
        if (!early_san) {
            // Code coverage instrumentation.
            if (options->sancov) {
                module_pm.addPass(SanitizerCoveragePass(getSanCovOptions(options->coverage)));
            }

            // Thread sanitizer
            if (options->tsan) {
                module_pm.addPass(ModuleThreadSanitizerPass());
                module_pm.addPass(createModuleToFunctionPassAdaptor(ThreadSanitizerPass()));
            }
        }

        // Verify the output
        if (assertions_on) {
            module_pm.addPass(VerifierPass());
        }
    });

    ModulePassManager module_pm;
    // Initialize the PassManager
    if (opt_level == OptimizationLevel::O0) {
      module_pm = pass_builder.buildO0DefaultPipeline(opt_level, static_cast<ThinOrFullLTOPhase>(options->lto));
    } else if (options->lto) {
      module_pm = pass_builder.buildLTOPreLinkDefaultPipeline(opt_level);
    } else {
      module_pm = pass_builder.buildPerModuleDefaultPipeline(opt_level);
    }

    // Unfortunately we don't have new PM for code generation
    legacy::PassManager codegen_pm;
    codegen_pm.add(
      createTargetTransformInfoWrapperPass(target_machine.getTargetIRAnalysis()));

    if (dest_bin && !options->lto) {
        if (target_machine.addPassesToEmitFile(codegen_pm, *dest_bin, nullptr, CodeGenFileType::ObjectFile)) {
            *error_message = strdup("TargetMachine can't emit an object file");
            return true;
        }
    }
    if (dest_asm) {
        if (target_machine.addPassesToEmitFile(codegen_pm, *dest_asm, nullptr, CodeGenFileType::AssemblyFile)) {
            *error_message = strdup("TargetMachine can't emit an assembly file");
            return true;
        }
    }

    // Optimization phase
    module_pm.run(llvm_module, module_am);

    // A second vectorization attempt. Roc emits list bounds and uniqueness
    // checks as ordinary calls, so a loop that will end up as pure lane
    // arithmetic still holds a call when the pipeline's vectorizer runs, and
    // only the later passes that prove the check redundant leave it in a
    // shape a vectorizer would take. Running one after those passes catches
    // the loops the first attempt could not see.
    if (opt_level != OptimizationLevel::O0) {
        FunctionPassManager revectorize_pm;
        revectorize_pm.addPass(LoopVectorizePass());
        revectorize_pm.addPass(SLPVectorizerPass());
        revectorize_pm.addPass(InstCombinePass());
        revectorize_pm.addPass(SimplifyCFGPass());

        for (Function &function : llvm_module) {
            if (!function.isDeclaration()) {
                revectorize_pm.run(function, function_am);
            }
        }
    }

    if (options->lower_memory_intrinsics_to_loops) {
        FunctionPassManager lower_mem_pm;
        lower_mem_pm.addPass(LowerMemoryIntrinsicsPass(target_machine.getTargetIRAnalysis()));

        for (Function &function : llvm_module) {
            if (!function.isDeclaration()) {
                lower_mem_pm.run(function, function_am);
            }
        }

        if (assertions_on) {
            std::string verify_error;
            raw_string_ostream verify_stream(verify_error);
            if (verifyModule(llvm_module, &verify_stream)) {
                verify_stream.flush();
                *error_message = strdup(verify_error.c_str());
                return true;
            }
        }
    }

    // Code generation phase
    codegen_pm.run(llvm_module);

    if (options->llvm_ir_filename) {
        if (LLVMPrintModuleToFile(module_ref, options->llvm_ir_filename, error_message)) {
            return true;
        }
    }

    if (dest_bin && options->lto) {
        WriteBitcodeToFile(llvm_module, *dest_bin);
    }
    if (dest_bitcode) {
        WriteBitcodeToFile(llvm_module, *dest_bitcode);
    }

    // This must only happen once we know we've succeeded and will be returning `false`, because
    // this code `malloc`s memory which will become owned by the caller (in Zig code).
    if (options->time_report_out != nullptr) {
        std::string out_str;
        auto os = raw_string_ostream(out_str);
        TimerGroup::printAll(os);
        TimerGroup::clearAll();
        auto c_str = (char *)malloc(out_str.length() + 1);
        strcpy(c_str, out_str.c_str());
        *options->time_report_out = c_str;
    }
    return false;
}

ZIG_EXTERN_C bool ZigLLVMTargetMachineEmitToFile(LLVMTargetMachineRef targ_machine_ref, LLVMModuleRef module_ref,
    char **error_message, const ZigLLVMEmitOptions *options)
{
    TimePassesIsEnabled = options->time_report_out != nullptr;
    std::unique_ptr<raw_fd_ostream> dest_bin;
    if (openEmitFile(options->bin_filename, dest_bin, error_message)) return true;
    return emitWithBinStream(targ_machine_ref, module_ref, error_message, options, dest_bin.get(),
        options->bin_filename ? options->bin_filename : options->asm_filename);
}

ZIG_EXTERN_C bool ZigLLVMTargetMachineEmitObjectToMemory(LLVMTargetMachineRef targ_machine_ref, LLVMModuleRef module_ref,
    char **error_message, const ZigLLVMEmitOptions *options, char **out_bytes, size_t *out_len)
{
    TimePassesIsEnabled = options->time_report_out != nullptr;
    SmallVector<char, 0> object_buf;
    raw_svector_ostream dest_bin(object_buf);
    if (emitWithBinStream(targ_machine_ref, module_ref, error_message, options, &dest_bin, "<memory>")) {
        return true;
    }

    char *bytes = (char *)malloc(object_buf.size() > 0 ? object_buf.size() : 1);
    if (bytes == nullptr) {
        *error_message = strdup("out of memory copying the emitted object");
        return true;
    }
    memcpy(bytes, object_buf.data(), object_buf.size());
    *out_bytes = bytes;
    *out_len = object_buf.size();
    return false;
}

// Partitioned emission.
//
// Roc gives LLVM the whole program as one module, and LLVM optimizes and code-generates a
// module on one thread, so an optimized build of a large app (the 7,654-function Atlas
// desktop UI) spent about 12 minutes on one core. Like rustc's codegen units, this splits
// the module into parts that are optimized and code-generated independently, each in its
// own LLVMContext on its own thread, and written to its own object file.
//
// Splitting changes what the optimizer can see, so the split keeps the inlining that
// matters:
//  - A local function with a single caller (most of Roc's lambda-lifted procedures) stays
//    in its caller's part, so the inliner still sees its only call site. A part is capped at
//    half the average part size so one call tree cannot take most of the program.
//  - Functions marked `alwaysinline` and constant globals referenced from another part are
//    copied into the referencing part as `available_externally`: the optimizer may inline or
//    fold them, and drops the copy afterwards, so each still has one definition.
//  - Every other reference across parts becomes a call to (or load of) a hidden symbol.
//    Locals that need one are renamed with a suffix unique to the module, so they cannot
//    collide with another object's symbols at link time.
// The plan depends only on the module, never on the thread count, so the objects are the
// same for any number of workers.
namespace {

constexpr uint64_t kInstructionsPerPartition = 400000;
constexpr size_t kMaxPartitions = 16;

uint64_t globalSize(const GlobalValue &gv) {
    if (auto *f = dyn_cast<Function>(&gv)) return 1 + f->getInstructionCount();
    return 1;
}

void collectReferencedGlobals(const User *user, SmallVectorImpl<GlobalValue *> &out,
                              SmallPtrSetImpl<const Constant *> &seen_constants) {
    for (const Value *operand : user->operands()) {
        if (auto *gv = dyn_cast<GlobalValue>(operand)) {
            out.push_back(const_cast<GlobalValue *>(gv));
        } else if (auto *constant = dyn_cast<Constant>(operand)) {
            if (seen_constants.insert(constant).second) {
                collectReferencedGlobals(constant, out, seen_constants);
            }
        }
    }
}

// Whether the module uses something whose references cannot cross a part boundary.
bool moduleCanBePartitioned(const Module &module) {
    if (!module.getComdatSymbolTable().empty()) return false;
    if (!module.ifunc_empty()) return false;
    for (const GlobalVariable &gv : module.globals()) {
        if (gv.hasAppendingLinkage()) return false;
    }
    for (const Function &f : module) {
        for (const BasicBlock &block : f) {
            if (block.hasAddressTaken()) return false;
        }
    }
    return true;
}

struct PartitionPlan {
    std::vector<GlobalValue *> defs;
    std::vector<unsigned> owner;
    std::vector<std::vector<unsigned>> imports;
    unsigned partition_count = 1;
};

PartitionPlan planPartitions(Module &module, unsigned partition_count) {
    PartitionPlan plan;
    plan.partition_count = partition_count;
    DenseMap<const GlobalValue *, unsigned> index;
    for (GlobalValue &gv : module.global_values()) {
        if (gv.isDeclaration()) continue;
        index[&gv] = plan.defs.size();
        plan.defs.push_back(&gv);
    }
    const size_t def_count = plan.defs.size();

    // Distinct defined globals each definition references, and who references each.
    std::vector<SmallVector<unsigned, 8>> refs(def_count);
    std::vector<SmallVector<unsigned, 2>> users(def_count);
    for (unsigned i = 0; i < def_count; i++) {
        SmallVector<GlobalValue *, 32> raw;
        SmallPtrSet<const Constant *, 32> seen;
        if (auto *f = dyn_cast<Function>(plan.defs[i])) {
            for (const Instruction &inst : instructions(*f)) collectReferencedGlobals(&inst, raw, seen);
        } else {
            collectReferencedGlobals(plan.defs[i], raw, seen);
        }
        SmallPtrSet<GlobalValue *, 32> unique;
        for (GlobalValue *ref : raw) {
            auto it = index.find(ref);
            if (it == index.end() || it->second == i || !unique.insert(ref).second) continue;
            refs[i].push_back(it->second);
            users[it->second].push_back(i);
        }
    }

    // Clusters that must or should share a part.
    std::vector<unsigned> parent(def_count);
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](unsigned x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    std::vector<uint64_t> own_size(def_count);
    uint64_t total_size = 0;
    for (unsigned i = 0; i < def_count; i++) {
        own_size[i] = globalSize(*plan.defs[i]);
        total_size += own_size[i];
    }
    std::vector<uint64_t> cluster_size = own_size;
    auto unite = [&](unsigned a, unsigned b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        parent[a] = b;
        cluster_size[b] += cluster_size[a];
    };
    std::vector<unsigned> single_caller;
    for (unsigned i = 0; i < def_count; i++) {
        GlobalValue *gv = plan.defs[i];
        if (isa<GlobalAlias>(gv)) {
            for (unsigned ref : refs[i]) unite(i, ref);
        } else if (isa<GlobalVariable>(gv)) {
            if (users[i].size() == 1) unite(i, users[i][0]);
        } else if (gv->hasLocalLinkage() && users[i].size() == 1 && isa<Function>(plan.defs[users[i][0]])) {
            single_caller.push_back(i);
        }
    }
    // Smallest callees first, so a capped cluster keeps its many small helpers and leaves
    // out a few large subtrees.
    const uint64_t cluster_cap = std::max<uint64_t>(1, total_size / partition_count / 2);
    std::stable_sort(single_caller.begin(), single_caller.end(),
                     [&](unsigned a, unsigned b) { return own_size[a] < own_size[b]; });
    for (unsigned callee : single_caller) {
        unsigned a = find(callee), b = find(users[callee][0]);
        if (a != b && cluster_size[a] + cluster_size[b] <= cluster_cap) unite(a, b);
    }

    // Largest cluster first onto the least loaded part; ties keep definition order, so the
    // plan is deterministic.
    std::vector<unsigned> roots;
    for (unsigned i = 0; i < def_count; i++) {
        if (find(i) == i) roots.push_back(i);
    }
    std::stable_sort(roots.begin(), roots.end(),
                     [&](unsigned a, unsigned b) { return cluster_size[a] > cluster_size[b]; });
    using Load = std::pair<uint64_t, unsigned>;
    std::priority_queue<Load, std::vector<Load>, std::greater<Load>> loads;
    for (unsigned p = 0; p < partition_count; p++) loads.push({0, p});
    std::vector<unsigned> root_partition(def_count, 0);
    for (unsigned root : roots) {
        Load least = loads.top();
        loads.pop();
        root_partition[root] = least.second;
        loads.push({least.first + cluster_size[root], least.second});
    }
    plan.owner.resize(def_count);
    for (unsigned i = 0; i < def_count; i++) plan.owner[i] = root_partition[find(i)];

    // Copies of `alwaysinline` functions and constants that each part references.
    plan.imports.resize(partition_count);
    std::vector<unsigned> imported_into(def_count, UINT32_MAX);
    for (unsigned p = 0; p < partition_count; p++) {
        SmallVector<unsigned, 256> work;
        for (unsigned i = 0; i < def_count; i++) {
            if (plan.owner[i] == p) work.append(refs[i].begin(), refs[i].end());
        }
        while (!work.empty()) {
            unsigned ref = work.pop_back_val();
            if (plan.owner[ref] == p || imported_into[ref] == p) continue;
            GlobalValue *gv = plan.defs[ref];
            bool importable = false;
            if (auto *f = dyn_cast<Function>(gv)) {
                importable = f->hasFnAttribute(Attribute::AlwaysInline) && !f->isInterposable();
            } else if (auto *var = dyn_cast<GlobalVariable>(gv)) {
                importable = var->isConstant() && var->hasInitializer() && !var->isInterposable();
            }
            if (!importable) continue;
            imported_into[ref] = p;
            plan.imports[p].push_back(ref);
            work.append(refs[ref].begin(), refs[ref].end());
        }
    }

    // Locals referenced from another part need a symbol.
    std::vector<bool> needs_symbol(def_count, false);
    for (unsigned p = 0; p < partition_count; p++) {
        auto mark = [&](unsigned def) {
            for (unsigned ref : refs[def]) {
                if (plan.owner[ref] != p) needs_symbol[ref] = true;
            }
        };
        for (unsigned i = 0; i < def_count; i++) {
            if (plan.owner[i] == p) mark(i);
        }
        for (unsigned def : plan.imports[p]) mark(def);
    }
    uint64_t module_hash = 0xcbf29ce484222325ull;
    for (GlobalValue *gv : plan.defs) {
        if (gv->hasLocalLinkage()) continue;
        for (char c : gv->getName()) module_hash = (module_hash ^ (unsigned char)c) * 0x100000001b3ull;
    }
    const std::string suffix = ".roc_part." + utohexstr(module_hash);
    for (unsigned i = 0; i < def_count; i++) {
        GlobalValue *gv = plan.defs[i];
        if (!needs_symbol[i] || !gv->hasLocalLinkage()) continue;
        std::string name = (gv->hasName() ? gv->getName().str() : "anon." + std::to_string(i)) + suffix;
        gv->setName(name);
        gv->setLinkage(GlobalValue::ExternalLinkage);
        gv->setVisibility(GlobalValue::HiddenVisibility);
    }
    return plan;
}

// Part `p` of `module` as bitcode: its own definitions, imported copies as
// `available_externally`, and declarations of everything else it references.
void writePartitionBitcode(const Module &module, const PartitionPlan &plan, unsigned p,
                           SmallVectorImpl<char> &out) {
    DenseMap<const GlobalValue *, unsigned> wanted;
    for (unsigned i = 0; i < plan.defs.size(); i++) {
        if (plan.owner[i] == p) wanted[plan.defs[i]] = 0;
    }
    for (unsigned def : plan.imports[p]) wanted[plan.defs[def]] = 1;
    ValueToValueMapTy vmap;
    std::unique_ptr<Module> part = CloneModule(module, vmap, [&](const GlobalValue *gv) {
        return wanted.count(gv) != 0;
    });
    for (unsigned def : plan.imports[p]) {
        auto *copy = cast<GlobalValue>(vmap[plan.defs[def]]);
        copy->setLinkage(GlobalValue::AvailableExternallyLinkage);
    }
    raw_svector_ostream stream(out);
    WriteBitcodeToFile(*part, stream);
}

TargetMachine *cloneTargetMachine(const TargetMachine &tm) {
    return tm.getTarget().createTargetMachine(tm.getTargetTriple(), tm.getTargetCPU(),
                                              tm.getTargetFeatureString(), tm.Options,
                                              tm.getRelocationModel(), tm.getCodeModel(),
                                              tm.getOptLevel());
}

} // end anonymous namespace

ZIG_EXTERN_C size_t ZigLLVMPartitionCount(LLVMModuleRef module_ref, const ZigLLVMEmitOptions *options) {
    const Module &module = *unwrap(module_ref);
    // Debug info, sanitizers, LTO and the extra outputs keep the single-module path.
    if (options->is_debug || options->tsan || options->sancov || options->lto ||
        options->asm_filename || options->llvm_ir_filename || options->bitcode_filename ||
        options->time_report_out || options->ir_opt_level == ZigLLVMIROptimizationLevel_O0 ||
        std::getenv("ZIG_LLVM_TIME_TRACE_FILE")) {
        return 1;
    }
    if (!module.debug_compile_units().empty() || !moduleCanBePartitioned(module)) return 1;
    uint64_t total = 0;
    for (const Function &f : module) total += f.getInstructionCount();
    uint64_t parts = total / kInstructionsPerPartition;
    return (size_t)std::clamp<uint64_t>(parts, 1, kMaxPartitions);
}

ZIG_EXTERN_C bool ZigLLVMTargetMachineEmitPartitionedObjects(LLVMTargetMachineRef targ_machine_ref,
    LLVMModuleRef module_ref, char **error_message, const ZigLLVMEmitOptions *options,
    size_t partition_count, size_t thread_count, const char *const *object_paths)
{
    if (partition_count <= 1) {
        ZigLLVMEmitOptions single = *options;
        single.bin_filename = object_paths[0];
        return ZigLLVMTargetMachineEmitToFile(targ_machine_ref, module_ref, error_message, &single);
    }
    TimePassesIsEnabled = false;
    Module &module = *unwrap(module_ref);
    const TargetMachine &parent_tm = *reinterpret_cast<TargetMachine *>(targ_machine_ref);

    std::vector<SmallVector<char, 0>> bitcode(partition_count);
    {
        PartitionPlan plan = planPartitions(module, (unsigned)partition_count);
        for (unsigned p = 0; p < partition_count; p++) writePartitionBitcode(module, plan, p, bitcode[p]);
    }

    // Largest parts first, so the longest one starts at once.
    std::vector<unsigned> order(partition_count);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(),
                     [&](unsigned a, unsigned b) { return bitcode[a].size() > bitcode[b].size(); });

    std::atomic<size_t> next{0};
    std::mutex error_mutex;
    std::string first_error;
    auto worker = [&] {
        for (size_t k; (k = next.fetch_add(1)) < partition_count;) {
            unsigned p = order[k];
            LLVMContext context;
            Expected<std::unique_ptr<Module>> part = parseBitcodeFile(
                MemoryBufferRef(StringRef(bitcode[p].data(), bitcode[p].size()), "roc_part"), context);
            std::string error;
            if (!part) {
                error = toString(part.takeError());
            } else {
                std::unique_ptr<TargetMachine> tm(cloneTargetMachine(parent_tm));
                ZigLLVMEmitOptions part_options = *options;
                part_options.bin_filename = object_paths[p];
                std::unique_ptr<raw_fd_ostream> dest_bin;
                char *message = nullptr;
                if (openEmitFile(object_paths[p], dest_bin, &message) ||
                    emitWithBinStream(reinterpret_cast<LLVMTargetMachineRef>(tm.get()), wrap(part->get()),
                                      &message, &part_options, dest_bin.get(), object_paths[p])) {
                    error = message ? message : "LLVM failed to emit a partition";
                    free(message);
                }
            }
            SmallVector<char, 0>().swap(bitcode[p]);
            if (!error.empty()) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (first_error.empty()) first_error = error;
            }
        }
    };
    // LLVM recurses on large functions; give workers the main thread's 64 MiB stack.
    const unsigned workers = (unsigned)std::max<size_t>(1, std::min(thread_count, partition_count));
    std::vector<llvm::thread> threads;
    for (unsigned w = 1; w < workers; w++) threads.emplace_back(std::optional<unsigned>(64u << 20), worker);
    worker();
    for (llvm::thread &t : threads) t.join();

    if (!first_error.empty()) {
        *error_message = strdup(first_error.c_str());
        return true;
    }
    return false;
}

ZIG_EXTERN_C void ZigLLVMFreeEmittedObject(char *bytes) {
    free(bytes);
}

void ZigLLVMSetOptBisectLimit(LLVMContextRef context_ref, int limit) {
    static OptBisect opt_bisect;
    opt_bisect.setIntervals({0, limit});
    unwrap(context_ref)->setOptPassGate(opt_bisect);
}

struct ZigDiagnosticHandler : public DiagnosticHandler {
bool BrokenDebugInfo;
    ZigDiagnosticHandler() : BrokenDebugInfo(false) {}
    bool handleDiagnostics(const DiagnosticInfo &DI) override {
        // This dyn_cast should be casting to DiagnosticInfoIgnoringInvalidDebugMetadata
        // but DiagnosticInfoIgnoringInvalidDebugMetadata is treated as DiagnosticInfoDebugMetadataVersion
        // because of a bug in LLVM (see https://github.com/ziglang/zig/issues/19161).
        // After this is fixed add an additional check for DiagnosticInfoIgnoringInvalidDebugMetadata
        // but don't remove the current one as both indicate that debug info is broken.
        if (auto *Remark = dyn_cast<DiagnosticInfoDebugMetadataVersion>(&DI)) {
            BrokenDebugInfo = true;
        }
        return false;
    }
};

void ZigLLVMEnableBrokenDebugInfoCheck(LLVMContextRef context_ref) {
    unwrap(context_ref)->setDiagnosticHandler(std::make_unique<ZigDiagnosticHandler>());
}

bool ZigLLVMGetBrokenDebugInfo(LLVMContextRef context_ref) {
    return ((const ZigDiagnosticHandler*)
        unwrap(context_ref)->getDiagHandlerPtr())->BrokenDebugInfo;
}

void ZigLLVMParseCommandLineOptions(size_t argc, const char *const *argv) {
    cl::ParseCommandLineOptions(argc, argv);
}

// Initialize all LLVM targets for compilation
void ZigLLVMInitializeAllTargets() {
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmParsers();
    LLVMInitializeAllAsmPrinters();
}

bool ZigLLVMWriteImportLibrary(const char *def_path, unsigned int coff_machine,
    const char *output_lib_path, bool kill_at)
{
    COFF::MachineTypes machine = static_cast<COFF::MachineTypes>(coff_machine);

    auto bufOrErr = MemoryBuffer::getFile(def_path);
    if (!bufOrErr) {
        return false;
    }

    MemoryBuffer& buf = *bufOrErr.get();
    Expected<object::COFFModuleDefinition> def =
        object::parseCOFFModuleDefinition(buf, machine, /* MingwDef */ true);

    if (!def) {
        return true;
    }

    // The exports-juggling code below is ripped from LLVM's DlltoolDriver.cpp

    // If ExtName is set (if the "ExtName = Name" syntax was used), overwrite
    // Name with ExtName and clear ExtName. When only creating an import
    // library and not linking, the internal name is irrelevant. This avoids
    // cases where writeImportLibrary tries to transplant decoration from
    // symbol decoration onto ExtName.
    for (object::COFFShortExport& E : def->Exports) {
        if (!E.ExtName.empty()) {
            E.Name = E.ExtName;
            E.ExtName.clear();
        }
    }

    if (kill_at) {
        for (object::COFFShortExport& E : def->Exports) {
            if (!E.ImportName.empty() || (!E.Name.empty() && E.Name[0] == '?'))
                continue;
            if (machine == COFF::IMAGE_FILE_MACHINE_I386) {
                // By making sure E.SymbolName != E.Name for decorated symbols,
                // writeImportLibrary writes these symbols with the type
                // IMPORT_NAME_UNDECORATE.
                E.SymbolName = E.Name;
            }
            // Trim off the trailing decoration. Symbols will always have a
            // starting prefix here (either _ for cdecl/stdcall, @ for fastcall
            // or ? for C++ functions). Vectorcall functions won't have any
            // fixed prefix, but the function base name will still be at least
            // one char.
            E.Name = E.Name.substr(0, E.Name.find('@', 1));
        }
    }

    return static_cast<bool>(
        object::writeImportLibrary(def->OutputFile, output_lib_path,
                                   def->Exports, machine, /* MinGW */ true));
}

// Like ZigLLVMWriteArchive, but inputs that are themselves archives are
// flattened: their members are added to the output archive directly, so the
// result never contains nested archives.
bool ZigLLVMWriteArchiveFlattened(const char *archive_name, const char **file_names,
    size_t file_name_count, ZigLLVMArchiveKind archive_kind)
{
    std::vector<std::unique_ptr<MemoryBuffer>> live_buffers;
    std::vector<std::unique_ptr<object::Archive>> live_archives;
    std::vector<NewArchiveMember> new_members;
    for (size_t i = 0; i < file_name_count; i += 1) {
        ErrorOr<std::unique_ptr<MemoryBuffer>> buf_or =
            MemoryBuffer::getFile(file_names[i], /*IsText=*/false, /*RequiresNullTerminator=*/false);
        if (!buf_or) return true;
        std::unique_ptr<MemoryBuffer> buf = std::move(*buf_or);
        if (identify_magic(buf->getBuffer()) == file_magic::archive) {
            Error archive_err = Error::success();
            auto archive = std::make_unique<object::Archive>(buf->getMemBufferRef(), archive_err);
            if (archive_err) {
                consumeError(std::move(archive_err));
                return true;
            }
            Error iter_err = Error::success();
            for (const object::Archive::Child &child : archive->children(iter_err)) {
                Expected<NewArchiveMember> member =
                    NewArchiveMember::getOldMember(child, /*Deterministic=*/true);
                if (!member) {
                    consumeError(member.takeError());
                    return true;
                }
                // Members reference data inside buf; keep it alive until writeArchive.
                new_members.push_back(std::move(*member));
            }
            if (iter_err) {
                consumeError(std::move(iter_err));
                return true;
            }
            live_archives.push_back(std::move(archive));
            live_buffers.push_back(std::move(buf));
        } else {
            Expected<NewArchiveMember> member =
                NewArchiveMember::getFile(file_names[i], /*Deterministic=*/true);
            if (!member) {
                consumeError(member.takeError());
                return true;
            }
            member->MemberName = sys::path::filename(file_names[i]);
            new_members.push_back(std::move(*member));
        }
    }
    Error err = writeArchive(archive_name, new_members,
        SymtabWritingMode::NormalSymtab, static_cast<object::Archive::Kind>(archive_kind),
        /*Deterministic=*/true, /*Thin=*/false, nullptr);

    if (err) {
        consumeError(std::move(err));
        return true;
    }
    return false;
}

bool ZigLLVMWriteArchive(const char *archive_name, const char **file_names, size_t file_name_count,
    ZigLLVMArchiveKind archive_kind)
{
    SmallVector<NewArchiveMember, 4> new_members;
    for (size_t i = 0; i < file_name_count; i += 1) {
        Expected<NewArchiveMember> new_member = NewArchiveMember::getFile(file_names[i], true);
        Error err = new_member.takeError();
        if (err) {
            consumeError(std::move(err));
            return true;
        }
        new_members.push_back(std::move(*new_member));
    }
    Error err = writeArchive(archive_name, new_members,
        SymtabWritingMode::NormalSymtab, static_cast<object::Archive::Kind>(archive_kind), true, false, nullptr);

    if (err) {
        consumeError(std::move(err));
        return true;
    }
    return false;
}

// The header file in LLD 16 exposed these functions. As of 17 they are only
// exposed via a macro ("LLD_HAS_DRIVER") which I have copied and pasted the
// body of here so that you don't have to wonder what it is doing.
namespace lld {
    namespace coff {
        bool link(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS,
                llvm::raw_ostream &stderrOS, bool exitEarly, bool disableOutput);
    }
    namespace elf {
        bool link(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS,
                llvm::raw_ostream &stderrOS, bool exitEarly, bool disableOutput);
    }
    namespace macho {
        bool link(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS,
                llvm::raw_ostream &stderrOS, bool exitEarly, bool disableOutput);
    }
    namespace wasm {
        bool link(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS,
                llvm::raw_ostream &stderrOS, bool exitEarly, bool disableOutput);
    }
}

static bool runLldDriver(lld::Driver driver, int argc, const char **argv,
                         bool can_exit_early, bool disable_output) {
    std::vector<const char *> args(argv, argv + argc);

    // A frontend link allocates the CommonLinkerContext that unsafeLldMain
    // normally destroys. Roc invokes the frontend directly, so release it here
    // before another in-process link can observe the previous driver's state.
    const bool success = driver(args, llvm::outs(), llvm::errs(), can_exit_early, disable_output);
    lld::CommonLinkerContext::destroy();
    return success;
}

bool ZigLLDLinkCOFF(int argc, const char **argv, bool can_exit_early, bool disable_output) {
    return runLldDriver(&lld::coff::link, argc, argv, can_exit_early, disable_output);
}

bool ZigLLDLinkELF(int argc, const char **argv, bool can_exit_early, bool disable_output) {
    return runLldDriver(&lld::elf::link, argc, argv, can_exit_early, disable_output);
}

bool ZigLLDLinkMachO(int argc, const char **argv, bool can_exit_early, bool disable_output) {
    return runLldDriver(&lld::macho::link, argc, argv, can_exit_early, disable_output);
}

bool ZigLLDLinkWasm(int argc, const char **argv, bool can_exit_early, bool disable_output) {
    return runLldDriver(&lld::wasm::link, argc, argv, can_exit_early, disable_output);
}

static_assert((FloatABI::ABIType)ZigLLVMFloatABI_Default == FloatABI::ABIType::Default, "");
static_assert((FloatABI::ABIType)ZigLLVMFloatABI_Soft == FloatABI::ABIType::Soft, "");
static_assert((FloatABI::ABIType)ZigLLVMFloatABI_Hard == FloatABI::ABIType::Hard, "");

static_assert((object::Archive::Kind)ZigLLVMArchiveKind_GNU == object::Archive::Kind::K_GNU, "");
static_assert((object::Archive::Kind)ZigLLVMArchiveKind_GNU64 == object::Archive::Kind::K_GNU64, "");
static_assert((object::Archive::Kind)ZigLLVMArchiveKind_BSD == object::Archive::Kind::K_BSD, "");
static_assert((object::Archive::Kind)ZigLLVMArchiveKind_DARWIN == object::Archive::Kind::K_DARWIN, "");
static_assert((object::Archive::Kind)ZigLLVMArchiveKind_DARWIN64 == object::Archive::Kind::K_DARWIN64, "");
static_assert((object::Archive::Kind)ZigLLVMArchiveKind_COFF == object::Archive::Kind::K_COFF, "");
static_assert((object::Archive::Kind)ZigLLVMArchiveKind_AIXBIG == object::Archive::Kind::K_AIXBIG, "");
