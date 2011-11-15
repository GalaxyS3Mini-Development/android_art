/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../../CompilerInternals.h"
#include "ArmLIR.h"
#include "../Ralloc.h"

#include <string>

static const char* coreRegNames[16] = {
    "r0",
    "r1",
    "r2",
    "r3",
    "r4",
    "r5",
    "r6",
    "r7",
    "r8",
    "rSELF",
    "r10",
    "r11",
    "r12",
    "sp",
    "lr",
    "pc",
};


static const char* shiftNames[4] = {
    "lsl",
    "lsr",
    "asr",
    "ror"};

/* Decode and print a ARM register name */
STATIC char* decodeRegList(ArmOpcode opcode, int vector, char* buf)
{
    int i;
    bool printed = false;
    buf[0] = 0;
    for (i = 0; i < 16; i++, vector >>= 1) {
        if (vector & 0x1) {
            int regId = i;
            if (opcode == kThumbPush && i == 8) {
                regId = r14lr;
            } else if (opcode == kThumbPop && i == 8) {
                regId = r15pc;
            }
            if (printed) {
                sprintf(buf + strlen(buf), ", r%d", regId);
            } else {
                printed = true;
                sprintf(buf, "r%d", regId);
            }
        }
    }
    return buf;
}

STATIC char*  decodeFPCSRegList(int count, int base, char* buf)
{
    sprintf(buf, "s%d", base);
    for (int i = 1; i < count; i++) {
        sprintf(buf + strlen(buf), ", s%d",base + i);
    }
    return buf;
}

STATIC int expandImmediate(int value)
{
    int mode = (value & 0xf00) >> 8;
    u4 bits = value & 0xff;
    switch(mode) {
        case 0:
            return bits;
       case 1:
            return (bits << 16) | bits;
       case 2:
            return (bits << 24) | (bits << 8);
       case 3:
            return (bits << 24) | (bits << 16) | (bits << 8) | bits;
      default:
            break;
    }
    bits = (bits | 0x80) << 24;
    return bits >> (((value & 0xf80) >> 7) - 8);
}

const char* ccNames[] = {"eq","ne","cs","cc","mi","pl","vs","vc",
                         "hi","ls","ge","lt","gt","le","al","nv"};
/*
 * Interpret a format string and build a string no longer than size
 * See format key in Assemble.c.
 */
STATIC std::string buildInsnString(const char* fmt, ArmLIR* lir, unsigned char* baseAddr)
{
    std::string buf;
    int i;
    const char* fmtEnd = &fmt[strlen(fmt)];
    char tbuf[256];
    const char* name;
    char nc;
    while (fmt < fmtEnd) {
        int operand;
        if (*fmt == '!') {
            fmt++;
            DCHECK_LT(fmt, fmtEnd);
            nc = *fmt++;
            if (nc=='!') {
                strcpy(tbuf, "!");
            } else {
               DCHECK_LT(fmt, fmtEnd);
               DCHECK_LT((unsigned)(nc-'0'), 4U);
               operand = lir->operands[nc-'0'];
               switch(*fmt++) {
                   case 'H':
                       if (operand != 0) {
                           sprintf(tbuf, ", %s %d",shiftNames[operand & 0x3],
                                   operand >> 2);
                       } else {
                           strcpy(tbuf,"");
                       }
                       break;
                   case 'B':
                       switch (operand) {
                           case kSY:
                               name = "sy";
                               break;
                           case kST:
                               name = "st";
                               break;
                           case kISH:
                               name = "ish";
                               break;
                           case kISHST:
                               name = "ishst";
                               break;
                           case kNSH:
                               name = "nsh";
                               break;
                           case kNSHST:
                               name = "shst";
                               break;
                           default:
                               name = "DecodeError2";
                               break;
                       }
                       strcpy(tbuf, name);
                       break;
                   case 'b':
                       strcpy(tbuf,"0000");
                       for (i=3; i>= 0; i--) {
                           tbuf[i] += operand & 1;
                           operand >>= 1;
                       }
                       break;
                   case 'n':
                       operand = ~expandImmediate(operand);
                       sprintf(tbuf,"%d [%#x]", operand, operand);
                       break;
                   case 'm':
                       operand = expandImmediate(operand);
                       sprintf(tbuf,"%d [%#x]", operand, operand);
                       break;
                   case 's':
                       sprintf(tbuf,"s%d",operand & FP_REG_MASK);
                       break;
                   case 'S':
                       sprintf(tbuf,"d%d",(operand & FP_REG_MASK) >> 1);
                       break;
                   case 'h':
                       sprintf(tbuf,"%04x", operand);
                       break;
                   case 'M':
                   case 'd':
                       sprintf(tbuf,"%d", operand);
                       break;
                   case 'C':
                       sprintf(tbuf,"%s",coreRegNames[operand]);
                       break;
                   case 'E':
                       sprintf(tbuf,"%d", operand*4);
                       break;
                   case 'F':
                       sprintf(tbuf,"%d", operand*2);
                       break;
                   case 'c':
                       strcpy(tbuf, ccNames[operand]);
                       break;
                   case 't':
                       sprintf(tbuf,"0x%08x (L%p)",
                               (int) baseAddr + lir->generic.offset + 4 +
                               (operand << 1),
                               lir->generic.target);
                       break;
                   case 'u': {
                       int offset_1 = lir->operands[0];
                       int offset_2 = NEXT_LIR(lir)->operands[0];
                       intptr_t target =
                           ((((intptr_t) baseAddr + lir->generic.offset + 4) &
                            ~3) + (offset_1 << 21 >> 9) + (offset_2 << 1)) &
                           0xfffffffc;
                       sprintf(tbuf, "%p", (void *) target);
                       break;
                    }

                   /* Nothing to print for BLX_2 */
                   case 'v':
                       strcpy(tbuf, "see above");
                       break;
                   case 'R':
                       decodeRegList(lir->opcode, operand, tbuf);
                       break;
                   case 'P':
                       decodeFPCSRegList(operand, 16, tbuf);
                       break;
                   case 'Q':
                       decodeFPCSRegList(operand, 0, tbuf);
                       break;
                   default:
                       strcpy(tbuf,"DecodeError1");
                       break;
                }
                buf += tbuf;
            }
        } else {
           buf += *fmt++;
        }
    }
    return buf;
}

void oatDumpResourceMask(LIR* lir, u8 mask, const char* prefix)
{
    char buf[256];
    buf[0] = 0;
    ArmLIR* armLIR = (ArmLIR*) lir;

    if (mask == ENCODE_ALL) {
        strcpy(buf, "all");
    } else {
        char num[8];
        int i;

        for (i = 0; i < kRegEnd; i++) {
            if (mask & (1ULL << i)) {
                sprintf(num, "%d ", i);
                strcat(buf, num);
            }
        }

        if (mask & ENCODE_CCODE) {
            strcat(buf, "cc ");
        }
        if (mask & ENCODE_FP_STATUS) {
            strcat(buf, "fpcc ");
        }

        /* Memory bits */
        if (armLIR && (mask & ENCODE_DALVIK_REG)) {
            sprintf(buf + strlen(buf), "dr%d%s", armLIR->aliasInfo & 0xffff,
                    (armLIR->aliasInfo & 0x80000000) ? "(+1)" : "");
        }
        if (mask & ENCODE_LITERAL) {
            strcat(buf, "lit ");
        }

        if (mask & ENCODE_HEAP_REF) {
            strcat(buf, "heap ");
        }
        if (mask & ENCODE_MUST_NOT_ALIAS) {
            strcat(buf, "noalias ");
        }
    }
    if (buf[0]) {
        LOG(INFO) << prefix << ": " << buf;
    }
}

/*
 * Debugging macros
 */
#define DUMP_RESOURCE_MASK(X)
#define DUMP_SSA_REP(X)

/* Pretty-print a LIR instruction */
void oatDumpLIRInsn(CompilationUnit* cUnit, LIR* arg, unsigned char* baseAddr)
{
    ArmLIR* lir = (ArmLIR*) arg;
    int offset = lir->generic.offset;
    int dest = lir->operands[0];
    const bool dumpNop = false;

    /* Handle pseudo-ops individually, and all regular insns as a group */
    switch(lir->opcode) {
        case kArmPseudoMethodEntry:
            LOG(INFO) << "-------- method entry " <<
                art::PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
            break;
        case kArmPseudoMethodExit:
            LOG(INFO) << "-------- Method_Exit";
            break;
        case kArmPseudoBarrier:
            LOG(INFO) << "-------- BARRIER";
            break;
        case kArmPseudoExtended:
            LOG(INFO) << "-------- " << (char* ) dest;
            break;
        case kArmPseudoSSARep:
            DUMP_SSA_REP(LOG(INFO) << "-------- kMirOpPhi: " <<  (char* ) dest);
            break;
        case kArmPseudoEntryBlock:
            LOG(INFO) << "-------- entry offset: 0x" << std::hex << dest;
            break;
        case kArmPseudoDalvikByteCodeBoundary:
            LOG(INFO) << "-------- dalvik offset: 0x" << std::hex <<
                 lir->generic.dalvikOffset << " @ " << (char* )lir->operands[0];
            break;
        case kArmPseudoExitBlock:
            LOG(INFO) << "-------- exit offset: 0x" << std::hex << dest;
            break;
        case kArmPseudoPseudoAlign4:
            LOG(INFO) << (intptr_t)baseAddr + offset << " (0x" << std::hex <<
                offset << "): .align4";
            break;
        case kArmPseudoEHBlockLabel:
            LOG(INFO) << "Exception_Handling:";
            break;
        case kArmPseudoTargetLabel:
        case kArmPseudoNormalBlockLabel:
            LOG(INFO) << "L" << (intptr_t)lir << ":";
            break;
        case kArmPseudoThrowTarget:
            LOG(INFO) << "LT" << (intptr_t)lir << ":";
            break;
        case kArmPseudoSuspendTarget:
            LOG(INFO) << "LS" << (intptr_t)lir << ":";
            break;
        case kArmPseudoCaseLabel:
            LOG(INFO) << "LC" << (intptr_t)lir << ": Case target 0x" <<
                std::hex << lir->operands[0] << "|" << std::dec <<
                lir->operands[0];
            break;
        default:
            if (lir->flags.isNop && !dumpNop) {
                break;
            } else {
                std::string op_name(buildInsnString(EncodingMap[lir->opcode].name, lir, baseAddr));
                std::string op_operands(buildInsnString(EncodingMap[lir->opcode].fmt, lir, baseAddr));
                LOG(INFO) << StringPrintf("%p (%04x): %-9s%s%s%s", baseAddr + offset, offset,
                    op_name.c_str(), op_operands.c_str(), lir->flags.isNop ? "(nop)" : "",
                    lir->flags.squashed ? "(squashed)" : "");
            }
            break;
    }

    if (lir->useMask && (!lir->flags.isNop || dumpNop)) {
        DUMP_RESOURCE_MASK(oatDumpResourceMask((LIR* ) lir,
                                               lir->useMask, "use"));
    }
    if (lir->defMask && (!lir->flags.isNop || dumpNop)) {
        DUMP_RESOURCE_MASK(oatDumpResourceMask((LIR* ) lir,
                                               lir->defMask, "def"));
    }
}

void oatDumpPromotionMap(CompilationUnit *cUnit)
{
    for (int i = 0; i < cUnit->numDalvikRegisters; i++) {
        PromotionMap vRegMap = cUnit->promotionMap[i];
        char buf[100];
        if (vRegMap.fpLocation == kLocPhysReg) {
            snprintf(buf, 100, " : s%d", vRegMap.fpReg & FP_REG_MASK);
        } else {
            buf[0] = 0;
        }
        char buf2[100];
        snprintf(buf2, 100, "V[%02d] -> %s%d%s", i,
                 vRegMap.coreLocation == kLocPhysReg ?
                 "r" : "SP+", vRegMap.coreLocation == kLocPhysReg ?
                 vRegMap.coreReg : oatSRegOffset(cUnit, i), buf);
        LOG(INFO) << buf2;
    }
}

void oatDumpFullPromotionMap(CompilationUnit *cUnit)
{
    for (int i = 0; i < cUnit->numDalvikRegisters; i++) {
        PromotionMap vRegMap = cUnit->promotionMap[i];
        LOG(INFO) << i << " -> " << "CL:" << (int)vRegMap.coreLocation <<
            ", CR:" << (int)vRegMap.coreReg << ", FL:" <<
            (int)vRegMap.fpLocation << ", FR:" << (int)vRegMap.fpReg <<
            ", - " << (int)vRegMap.firstInPair;
    }
}

/* Dump instructions and constant pool contents */
void oatCodegenDump(CompilationUnit* cUnit)
{
    LOG(INFO) << "/*";
    LOG(INFO) << "Dumping LIR insns for "
        << art::PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
    LIR* lirInsn;
    ArmLIR* armLIR;
    int insnsSize = cUnit->insnsSize;

    LOG(INFO) << "Regs (excluding ins) : " << cUnit->numRegs;
    LOG(INFO) << "Ins                  : " << cUnit->numIns;
    LOG(INFO) << "Outs                 : " << cUnit->numOuts;
    LOG(INFO) << "CoreSpills           : " << cUnit->numCoreSpills;
    LOG(INFO) << "FPSpills             : " << cUnit->numFPSpills;
    LOG(INFO) << "Padding              : " << cUnit->numPadding;
    LOG(INFO) << "Frame size           : " << cUnit->frameSize;
    LOG(INFO) << "Start of ins         : " << cUnit->insOffset;
    LOG(INFO) << "Start of regs        : " << cUnit->regsOffset;
    LOG(INFO) << "code size is " << cUnit->totalSize <<
        " bytes, Dalvik size is " << insnsSize * 2;
    LOG(INFO) << "expansion factor: " <<
         (float)cUnit->totalSize / (float)(insnsSize * 2);
    oatDumpPromotionMap(cUnit);
    for (lirInsn = cUnit->firstLIRInsn; lirInsn; lirInsn = lirInsn->next) {
        oatDumpLIRInsn(cUnit, lirInsn, 0);
    }
    for (lirInsn = cUnit->classPointerList; lirInsn; lirInsn = lirInsn->next) {
        armLIR = (ArmLIR*) lirInsn;
        LOG(INFO) << StringPrintf("%x (%04x): .class (%s)",
            armLIR->generic.offset, armLIR->generic.offset,
            ((CallsiteInfo *) armLIR->operands[0])->classDescriptor);
    }
    for (lirInsn = cUnit->literalList; lirInsn; lirInsn = lirInsn->next) {
        armLIR = (ArmLIR*) lirInsn;
        LOG(INFO) << StringPrintf("%x (%04x): .word (%#x)",
            armLIR->generic.offset, armLIR->generic.offset, armLIR->operands[0]);
    }

    const art::DexFile::MethodId& method_id =
        cUnit->dex_file->GetMethodId(cUnit->method_idx);
    std::string signature = cUnit->dex_file->GetMethodSignature(method_id);
    std::string name = cUnit->dex_file->GetMethodName(method_id);
    std::string descriptor =
        cUnit->dex_file->GetMethodDeclaringClassDescriptor(method_id);

    // Dump mapping table
    if (cUnit->mappingTable.size() > 0) {
        std::string line(StringPrintf("\n    MappingTable %s%s_%s_mappingTable[%d] = {",
            descriptor.c_str(), name.c_str(), signature.c_str(), cUnit->mappingTable.size()));
        std::replace(line.begin(), line.end(), ';', '_');
        LOG(INFO) << line;
        for (uint32_t i = 0; i < cUnit->mappingTable.size(); i+=2) {
            line = StringPrintf("        {0x%08x, 0x%04x},",
                cUnit->mappingTable[i], cUnit->mappingTable[i+1]);
            LOG(INFO) << line;
        }
        LOG(INFO) <<"    };\n\n";
    }
}
