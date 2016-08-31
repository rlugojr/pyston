#include <memory>
#include <unordered_map>
#include <vector>
#include <unordered_set>

#include "gtest/gtest.h"

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "codegen/irgen/future.h"
#include "codegen/osrentry.h"
#include "codegen/parser.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "runtime/types.h"
#include "unittests.h"

using namespace pyston;

class AnalysisTest : public ::testing::Test {
protected:
    static void SetUpTestCase() {
        Py_Initialize();
    }
};

// this test use functions (VRegInfo::getVReg) which are only available in a debug build
#ifndef NDEBUG
TEST_F(AnalysisTest, augassign) {
    const std::string fn("test/unittests/analysis_listcomp.py");
    AST_Module* module = caching_parse_file(fn.c_str(), 0);
    assert(module);

    FutureFlags future_flags = getFutureFlags(module->body, fn.c_str());

    auto scoping = std::make_shared<ScopingAnalysis>(module, true);
    computeAllCFGs(module, true, future_flags, boxString(fn), NULL);

    assert(module->body[0]->type == AST_TYPE::FunctionDef);
    AST_FunctionDef* func = static_cast<AST_FunctionDef*>(module->body[0]);

    ScopeInfo* scope_info = scoping->getScopeInfoForNode(func);

    ASSERT_NE(scope_info->getScopeTypeOfName(module->interned_strings->get("a")), ScopeInfo::VarScopeType::GLOBAL);
    ASSERT_FALSE(scope_info->getScopeTypeOfName(module->interned_strings->get("b")) == ScopeInfo::VarScopeType::GLOBAL);

    ParamNames param_names(func, *module->interned_strings.get());

    // Hack to get at the cfg:
    auto node = module->md->source->cfg->blocks[0]->body[0];
    CFG* cfg = ast_cast<AST_MakeFunction>(ast_cast<AST_Assign>(node)->value)->function_def->md->source->cfg;

    std::unique_ptr<LivenessAnalysis> liveness = computeLivenessInfo(cfg);
    auto&& vregs = cfg->getVRegInfo();

    //cfg->print();

    for (CFGBlock* block : cfg->blocks) {
        //printf("%d\n", block->idx);
        if (block->body.back()->type != AST_TYPE::Return)
            ASSERT_TRUE(liveness->isLiveAtEnd(vregs.getVReg(module->interned_strings->get("a")), block));
    }

    std::unique_ptr<PhiAnalysis> phis = computeRequiredPhis(ParamNames(func, *module->interned_strings.get()), cfg, liveness.get());
}

void doOsrTest(bool is_osr, bool i_maybe_undefined) {
    const std::string fn("test/unittests/analysis_osr.py");
    AST_Module* module = caching_parse_file(fn.c_str(), 0);
    assert(module);

    ParamNames param_names(module, *module->interned_strings.get());

    assert(module->body[0]->type == AST_TYPE::FunctionDef);
    AST_FunctionDef* func = static_cast<AST_FunctionDef*>(module->body[0]);

    auto scoping = std::make_shared<ScopingAnalysis>(module, true);
    ScopeInfo* scope_info = scoping->getScopeInfoForNode(func);

    FutureFlags future_flags = getFutureFlags(module->body, fn.c_str());

    computeAllCFGs(module, true, future_flags, boxString(fn), NULL);

    // Hack to get at the cfg:
    auto node = module->md->source->cfg->blocks[0]->body[0];
    auto md = ast_cast<AST_MakeFunction>(ast_cast<AST_Assign>(node)->value)->function_def->md;
    CFG* cfg = md->source->cfg;
    std::unique_ptr<LivenessAnalysis> liveness = computeLivenessInfo(cfg);

    // cfg->print();

    auto&& vregs = cfg->getVRegInfo();

    InternedString i_str = module->interned_strings->get("i");
    InternedString idi_str = module->interned_strings->get("!is_defined_i");
    InternedString iter_str = module->interned_strings->get("#iter_3");

    CFGBlock* loop_backedge = cfg->blocks[5];
    ASSERT_EQ(6, loop_backedge->idx);
    ASSERT_EQ(1, loop_backedge->body.size());

    ASSERT_EQ(AST_TYPE::Jump, loop_backedge->body[0]->type);
    AST_Jump* backedge = ast_cast<AST_Jump>(loop_backedge->body[0]);
    ASSERT_LE(backedge->target->idx, loop_backedge->idx);

    std::unique_ptr<PhiAnalysis> phis;

    if (is_osr) {
        int vreg = vregs.getVReg(i_str);
        OSREntryDescriptor* entry_descriptor = OSREntryDescriptor::create(md, backedge, CXX);
        // need to set it to non-null
        ConcreteCompilerType* fake_type = (ConcreteCompilerType*)1;
        entry_descriptor->args[vreg] = fake_type;
        if (i_maybe_undefined)
            entry_descriptor->potentially_undefined.set(vreg);
        entry_descriptor->args[vregs.getVReg(iter_str)] = fake_type;
        phis = computeRequiredPhis(entry_descriptor, liveness.get());
    } else {
        phis = computeRequiredPhis(ParamNames(func, *module->interned_strings), cfg, liveness.get());
    }

    // First, verify that we require phi nodes for the block we enter into.
    // This is somewhat tricky since the osr entry represents an extra entry
    // into the BB which the analysis might not otherwise track.

    auto required_phis = phis->getAllRequiredFor(backedge->target);
    EXPECT_EQ(1, required_phis[vregs.getVReg(i_str)]);
    EXPECT_EQ(1, required_phis[vregs.getVReg(iter_str)]);
    EXPECT_EQ(2, required_phis.numSet());

    EXPECT_EQ(!is_osr || i_maybe_undefined, phis->isPotentiallyUndefinedAt(vregs.getVReg(i_str), backedge->target));
    EXPECT_FALSE(phis->isPotentiallyUndefinedAt(vregs.getVReg(iter_str), backedge->target));
    EXPECT_EQ(!is_osr || i_maybe_undefined, phis->isPotentiallyUndefinedAfter(vregs.getVReg(i_str), loop_backedge));
    EXPECT_FALSE(phis->isPotentiallyUndefinedAfter(vregs.getVReg(iter_str), loop_backedge));

    // Now, let's verify that we don't need a phi after the loop

    CFGBlock* if_join = cfg->blocks[7];
    ASSERT_EQ(8, if_join->idx);
    ASSERT_EQ(2, if_join->predecessors.size());

    if (is_osr)
        EXPECT_EQ(0, phis->getAllRequiredFor(if_join).numSet());
    else
        EXPECT_EQ(1, phis->getAllRequiredFor(if_join).numSet());
}

TEST_F(AnalysisTest, osr_initial) {
    doOsrTest(false, false);
}
TEST_F(AnalysisTest, osr1) {
    doOsrTest(true, false);
}
TEST_F(AnalysisTest, osr2) {
    doOsrTest(true, true);
}
#endif
