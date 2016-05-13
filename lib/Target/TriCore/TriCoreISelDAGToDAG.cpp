//===-- TriCoreISelDAGToDAG.cpp - A dag to dag inst selector for TriCore --===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the TriCore target.
//
//===----------------------------------------------------------------------===//

#include "TriCore.h"
#include "TriCoreTargetMachine.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "TriCoreInstrInfo.h"
#include "TriCoreCallingConvHook.h"

#define DEBUG_TYPE "tricore-isel"

using namespace llvm;


namespace {
struct TriCoreISelAddressMode {
  enum {
    RegBase,
    FrameIndexBase
  } BaseType;

  struct {            // This is really a union, discriminated by BaseType!
    SDValue Reg;
    int FrameIndex;
  } Base;

  int64_t Disp;
  const GlobalValue *GV;
  const Constant *CP;
  const BlockAddress *BlockAddr;
  const char *ES;
  int JT;
  unsigned Align;    // CP alignment.

  TriCoreISelAddressMode()
  : BaseType(RegBase), Disp(0), GV(nullptr), CP(nullptr),
    BlockAddr(nullptr), ES(nullptr), JT(-1), Align(0) {
  }

  bool hasSymbolicDisplacement() const {
    return GV != nullptr || CP != nullptr || ES != nullptr || JT != -1;
  }

  void dump() {
    errs() << "rriCoreISelAddressMode " << this << '\n';
    if (BaseType == RegBase && Base.Reg.getNode() != nullptr) {
      errs() << "Base.Reg ";
      Base.Reg.getNode()->dump();
    } else if (BaseType == FrameIndexBase) {
      errs() << " Base.FrameIndex " << Base.FrameIndex << '\n';
    }
    errs() << " Disp " << Disp << '\n';
    if (GV) {
      errs() << "GV ";
      GV->dump();
    } else if (CP) {
      errs() << " CP ";
      CP->dump();
      errs() << " Align" << Align << '\n';
    } else if (ES) {
      errs() << "ES ";
      errs() << ES << '\n';
    } else if (JT != -1)
      errs() << " JT" << JT << " Align" << Align << '\n';
  }
};
}

/// TriCoreDAGToDAGISel - TriCore specific code to select TriCore machine
/// instructions for SelectionDAG operations.
///
namespace {
class TriCoreDAGToDAGISel : public SelectionDAGISel {
  const TriCoreSubtarget &Subtarget;


public:
  explicit TriCoreDAGToDAGISel(TriCoreTargetMachine &TM, CodeGenOpt::Level OptLevel)
  : SelectionDAGISel(TM, OptLevel), Subtarget(*TM.getSubtargetImpl()) {}

  SDNode *Select(SDNode *N);
  SDNode *SelectConstant(SDNode *N);

  bool SelectAddr(SDValue Addr, SDValue &Base, SDValue &Offset);
  bool SelectAddr_new(SDValue N, SDValue &Base, SDValue &Disp);
  bool MatchAddress(SDValue N, TriCoreISelAddressMode &AM);
  bool MatchWrapper(SDValue N, TriCoreISelAddressMode &AM);
  bool MatchAddressBase(SDValue N, TriCoreISelAddressMode &AM);
  static bool isPointer();
  virtual const char *getPassName() const {
    return "TriCore DAG->DAG Pattern Instruction Selection";
  }

  static bool ptyType;

  // Include the pieces autogenerated from the target description.
#include "TriCoreGenDAGISel.inc"
};

} // end anonymous namespace

bool TriCoreDAGToDAGISel::ptyType = false;
bool TriCoreDAGToDAGISel::isPointer() { return ptyType;}
/// MatchWrapper - Try to match MSP430ISD::Wrapper node into an addressing mode.
/// These wrap things that will resolve down into a symbol reference.  If no
/// match is possible, this returns true, otherwise it returns false.
bool TriCoreDAGToDAGISel::MatchWrapper(SDValue N, TriCoreISelAddressMode &AM) {
  // If the addressing mode already has a symbol as the displacement, we can
  // never match another symbol.
  if (AM.hasSymbolicDisplacement()) {
    DEBUG(errs().changeColor(raw_ostream::YELLOW,1);
    errs() <<"hasSymbolicDisplacement\n";
    N.dump();
    errs().changeColor(raw_ostream::WHITE,0); );
    return true;
  }

  SDValue N0 = N.getOperand(0);

  DEBUG(errs() << "Match Wrapper N => ";
  N.dump();
  errs()<< "N0 => "; N0.dump(); );

  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(N0)) {
    AM.GV = G->getGlobal();
    AM.Disp += G->getOffset();
    DEBUG(errs() << "MatchWrapper->Displacement: " << AM.Disp );
    //AM.SymbolFlags = G->getTargetFlags();
  }
  return false;
}

/// MatchAddressBase - Helper for MatchAddress. Add the specified node to the
/// specified addressing mode without any further recursion.
bool TriCoreDAGToDAGISel::MatchAddressBase(SDValue N, TriCoreISelAddressMode &AM) {
  // Is the base register already occupied?
  if (AM.BaseType != TriCoreISelAddressMode::RegBase || AM.Base.Reg.getNode()) {
    // If so, we cannot select it.
    return true;
  }

  // Default, generate it as a register.
  AM.BaseType = TriCoreISelAddressMode::RegBase;
  AM.Base.Reg = N;
  return false;
}


bool TriCoreDAGToDAGISel::MatchAddress(SDValue N, TriCoreISelAddressMode &AM) {
  DEBUG(errs() << "MatchAddress: "; AM.dump());
  DEBUG(errs() << "Node: "; N.dump());


  switch (N.getOpcode()) {
  default: break;
  case ISD::Constant: {

    uint64_t Val = cast<ConstantSDNode>(N)->getSExtValue();
    AM.Disp += Val;
    DEBUG(errs() << "MatchAddress->Disp: " << AM.Disp ;);
    return false;
  }

  case TriCoreISD::Wrapper:
    if (!MatchWrapper(N, AM))
      return false;
    break;

  case ISD::FrameIndex:
    if (AM.BaseType == TriCoreISelAddressMode::RegBase
        && AM.Base.Reg.getNode() == nullptr) {
      AM.BaseType = TriCoreISelAddressMode::FrameIndexBase;
      AM.Base.FrameIndex = cast<FrameIndexSDNode>(N)->getIndex();
      return false;
    }
    break;

  case ISD::ADD: {
    TriCoreISelAddressMode Backup = AM;
    if (!MatchAddress(N.getNode()->getOperand(0), AM) &&
        !MatchAddress(N.getNode()->getOperand(1), AM))
      return false;
    AM = Backup;
    if (!MatchAddress(N.getNode()->getOperand(1), AM) &&
        !MatchAddress(N.getNode()->getOperand(0), AM))
      return false;
    AM = Backup;

    break;
  }

  case ISD::OR:
    // Handle "X | C" as "X + C" iff X is known to have C bits clear.
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      TriCoreISelAddressMode Backup = AM;
      uint64_t Offset = CN->getSExtValue();
      // Start with the LHS as an addr mode.
      if (!MatchAddress(N.getOperand(0), AM) &&
          // Address could not have picked a GV address for the displacement.
          AM.GV == nullptr &&
          // Check to see if the LHS & C is zero.
          CurDAG->MaskedValueIsZero(N.getOperand(0), CN->getAPIntValue())) {
        AM.Disp += Offset;
        return false;
      }
      AM = Backup;
    }
    break;
  }

  return MatchAddressBase(N, AM);
}

/// SelectAddr - returns true if it is able pattern match an addressing mode.
/// It returns the operands which make up the maximal addressing mode it can
/// match by reference.
bool TriCoreDAGToDAGISel::SelectAddr_new(SDValue N,
    SDValue &Base, SDValue &Disp) {
  TriCoreISelAddressMode AM;

  DEBUG( errs().changeColor(raw_ostream::YELLOW,1);
  N.dump();
  errs().changeColor(raw_ostream::WHITE,0) );


  if (MatchAddress(N, AM))
    return false;

  EVT VT = N.getValueType();
  if (AM.BaseType == TriCoreISelAddressMode::RegBase) {
    DEBUG(errs() << "It's a reg base";);
    if (!AM.Base.Reg.getNode())
      AM.Base.Reg = CurDAG->getRegister(0, VT);
  }


  Base = (AM.BaseType == TriCoreISelAddressMode::FrameIndexBase)
                 ? CurDAG->getTargetFrameIndex(
                     AM.Base.FrameIndex,
                     getTargetLowering()->getPointerTy(CurDAG->getDataLayout()))
                     : AM.Base.Reg;

  if (AM.GV) {
    DEBUG(errs() <<"AM.GV" );
    //GlobalAddressSDNode *gAdd = dyn_cast<GlobalAddressSDNode>(N.getOperand(0));
    Base = N;
    Disp = CurDAG->getTargetConstant(AM.Disp, N, MVT::i32);
  }
  else {
    DEBUG(errs()<<"SelectAddr -> AM.Disp\n";
    errs()<< "SelectAddr -> Displacement: " << AM.Disp; );
    Disp = CurDAG->getTargetConstant(AM.Disp, SDLoc(N), MVT::i32);
  }


  return true;
}


bool TriCoreDAGToDAGISel::SelectAddr(SDValue Addr, SDValue &Base, SDValue &Offset) {


  return SelectAddr_new(Addr, Base, Offset);

  outs().changeColor(raw_ostream::GREEN,1);
  Addr.dump();
  outs() <<"Addr Opcode: " << Addr.getOpcode() <<"\n";
  outs().changeColor(raw_ostream::WHITE,0);


  if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>(Addr)) {
    //    EVT PtrVT = getTargetLowering()->getPointerTy(*TM.getDataLayout());
    EVT PtrVT = getTargetLowering()->getPointerTy(CurDAG->getDataLayout());
    Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), PtrVT);
    Offset = CurDAG->getTargetConstant(0, Addr, MVT::i32);
    //    outs().changeColor(raw_ostream::RED)<<"Selecting Frame!\n";
    //    outs().changeColor(raw_ostream::WHITE);

    return true;
  }


  outs().changeColor(raw_ostream::BLUE,1);
  Addr.dump();
  outs().changeColor(raw_ostream::WHITE,0);

  if (Addr.getOpcode() == ISD::TargetExternalSymbol ||
      Addr.getOpcode() == ISD::TargetGlobalAddress ||
      Addr.getOpcode() == ISD::TargetGlobalTLSAddress) {
    outs()<<"This is working!!!!!!!!!!!!!!\n";
    //Base = Addr;
    //Offset = CurDAG->getTargetConstant(gAdd->getOffset(), Addr, MVT::i32);
    return false;
  }

  Base = Addr;
  Offset = CurDAG->getTargetConstant(0, Addr, MVT::i32);
  return true;
}

// Returns one plus the index of the least significant
// 1-bit of x, or if x is zero, returns zero.
static int getFFS (unsigned v) { return __builtin_ffs(v);}

// Return the number of set bits
static int getNumSetBits(unsigned int v) {
  int c=0;
  v = v - ((v >> 1) & 0x55555555);                    // reuse input as temporary
  v = (v & 0x33333333) + ((v >> 2) & 0x33333333);     // temp
  c = (((v + (v >> 4)) & 0xF0F0F0F) * 0x1010101) >> 24; // count
  return c;
}

// Return the number of consecutive set bits
static int getNumConsecutiveOnes(int in) {
  int count = 0;
  while (in) {
    in = (in & (in << 1));
    count++;
  }
  return count;
}

static int ipow(int base)
{
    int result = 1;
    for (int i = 0; i<base; i++)
      result = result<<1;
    return result;
}

SDNode *TriCoreDAGToDAGISel::SelectConstant(SDNode *N) {
   // Make sure the immediate size is supported.
    ConstantSDNode *ConstVal = cast<ConstantSDNode>(N);
    uint64_t ImmVal = ConstVal->getZExtValue();
    int64_t ImmSVal = ConstVal->getSExtValue();

    if ( ConstVal->getValueType(0) == MVT::i64) {
      /*
       * In case, we get a 64 bit constant node, we first try to generate an
       * imask instruction. Only if it fails, then we proceed to generate
       * pseudo moves.
       */
      uint32_t lowerByte  = ImmVal & 0x00000000ffffffff;
      uint32_t higherByte = ImmVal>>32;
      uint64_t width = 0;

      outs()<<"higherByte: " << higherByte << "\n";
      outs()<<"lowerByte: " <<  lowerByte << "\n";
      if (ImmVal == 0) {
        SDValue _constVal = CurDAG->getTargetConstant(0, N, MVT::i32);
        SDValue _width = CurDAG->getTargetConstant(0, N, MVT::i32);
        SDValue _pos = CurDAG->getTargetConstant(0, N, MVT::i32);
        return CurDAG->getMachineNode(TriCore::IMASKrcpw, N, MVT::i64,
                    _constVal, _pos, _width);
      }

      // In case both bytes contain set bits then exit
      if(ImmSVal<0 || (higherByte!=0 && lowerByte!=0)) {
        outs()<< "exit\n";
        return SelectCode(N);
      }
      else if(higherByte==0 && lowerByte!=0) {
        uint64_t posLSB = getFFS(lowerByte) - 1;
        uint64_t numSetBits = getNumSetBits(lowerByte);
        uint64_t numConsecBits = getNumConsecutiveOnes(lowerByte);
        // In case the patch of set bits is not a mask then exit
        if (numSetBits != numConsecBits) return SelectCode(N);
        // In case the mask for the lower byte is > 0xf we exit
        if (numConsecBits > 4) return SelectCode(N);

        // In case we are dealing with the lower byte,
        // only Const4Val is set
        int64_t Const4Val = ipow(numConsecBits) - 1;
        outs()<<"posLSB: " << posLSB << "\n";
        outs()<<"ConstVal: " << Const4Val << "\n";

        SDValue _constVal = CurDAG->getTargetConstant(Const4Val, N, MVT::i32);
        SDValue _width = CurDAG->getTargetConstant(width, N, MVT::i32);
        SDValue _pos = CurDAG->getTargetConstant(posLSB, N, MVT::i32);

        return CurDAG->getMachineNode(TriCore::IMASKrcpw, N, MVT::i64,
            _constVal, _pos, _width);
      }
      else if (higherByte!=0 && lowerByte==0) {
        uint64_t posLSB = getFFS(higherByte) - 1;
        uint64_t numSetBits = getNumSetBits(higherByte);
        uint64_t numConsecBits = getNumConsecutiveOnes(higherByte);
        outs()<<"posLSB: " << posLSB << "\n";
        outs()<<"numConsecBits: " << numConsecBits << "\n";
        // In case the patch of set bits is not a mask then exit
        if (numSetBits != numConsecBits) return SelectCode(N);

        // As per data sheet: (pos  + width)>31 is undefined
        if ((posLSB + numConsecBits) > 31) return SelectCode(N);

        SDValue _constVal = CurDAG->getTargetConstant(0, N, MVT::i32);
        SDValue _width = CurDAG->getTargetConstant(numConsecBits, N, MVT::i32);
        SDValue _pos = CurDAG->getTargetConstant(posLSB, N, MVT::i32);

        return CurDAG->getMachineNode(TriCore::IMASKrcpw, N, MVT::i64,
            _constVal, _pos, _width);

      }

    }

//    if ((ImmVal & SupportedMask) != ImmVal) {
////      outs() <<" Immediate size not supported!\n";
//      return SelectCode(N);
//    }

    // Select the low part of the immediate move.
    uint64_t LoMask = 0xffff;
    uint64_t HiMask = 0xffff0000;
    uint64_t ImmLo = (ImmVal & LoMask);
    int64_t ImmSLo = (ImmSVal & LoMask) - 65536;

//    outs() << "SLo: " << ImmSLo << "\n";
    uint64_t ImmHi = (ImmVal & HiMask);
    SDValue ConstLo = CurDAG->getTargetConstant(ImmLo, N, MVT::i32);
    SDValue ConstSImm = CurDAG->getTargetConstant(ImmSVal, N, MVT::i32);
    SDValue ConstEImm = CurDAG->getTargetConstant(ImmVal, N, MVT::i32);
    SDValue ConstHi;

    int64_t ImmLo_ext64 = (int16_t)ImmLo;
    int64_t hiShift = (ImmSVal - ImmLo_ext64) >> 16;

    if (hiShift < 0)
      hiShift = 65536 + hiShift;

    ConstHi = CurDAG->getTargetConstant(hiShift, N, MVT::i32);

    MachineSDNode *Move;

    if ((ImmHi == 0) && ImmLo) {
      if (ImmSVal >=0 && ImmSVal < 32768)
        return CurDAG->getMachineNode(TriCore::MOVrlc, N, MVT::i32, ConstEImm);
      else if(ImmSVal >=32768 && ImmSVal < 65536)
        return CurDAG->getMachineNode(TriCore::MOVUrlc, N, MVT::i32, ConstEImm);

    }
    else if(ImmHi && (ImmLo == 0))
      Move = CurDAG->getMachineNode(TriCore::MOVHrlc, N, MVT::i32, ConstHi);
    else if((ImmHi == 0) && (ImmLo == 0))
      return CurDAG->getMachineNode(TriCore::MOVrlc, N, MVT::i32, ConstHi);
    else {

      Move = CurDAG->getMachineNode(TriCore::MOVHrlc, N, MVT::i32, ConstHi);

      if ( (ImmSVal >= -32768) && (ImmSVal < 0))
          return CurDAG->getMachineNode(TriCore::MOVrlc, N, MVT::i32, ConstSImm);

      if( (ImmSLo >= -8 && ImmSLo < 8 ) || ImmLo < 8)
        Move = CurDAG->getMachineNode(TriCore::ADDsrc, N, MVT::i32,
                                            SDValue(Move,0), ConstLo);
      else if(ImmLo >=8 && ImmLo < 256)
        Move = CurDAG->getMachineNode(TriCore::ADDrc, N, MVT::i32,
                                      SDValue(Move,0), ConstLo);
      else
        Move = CurDAG->getMachineNode(TriCore::ADDIrlc, N, MVT::i32,
                                              SDValue(Move,0), ConstLo);
      }

    return Move;
}

SDNode *TriCoreDAGToDAGISel::Select(SDNode *N) {


  SDLoc dl(N);
  // Dump information about the Node being selected
  DEBUG(errs().changeColor(raw_ostream::GREEN) << "Selecting: ");
  DEBUG(N->dump(CurDAG));
  DEBUG(errs() << "\n");
  switch (N->getOpcode()) {
  case ISD::Constant:
    return SelectConstant(N);
  case ISD::FrameIndex: {
    int FI = cast<FrameIndexSDNode>(N)->getIndex();
    SDValue TFI = CurDAG->getTargetFrameIndex(FI, MVT::i32);
    if (N->hasOneUse()) {
      return CurDAG->SelectNodeTo(N, TriCore::ADDrc, MVT::i32, TFI,
          CurDAG->getTargetConstant(0, dl, MVT::i32));
    }
    return CurDAG->getMachineNode(TriCore::ADDrc, dl, MVT::i32, TFI,
        CurDAG->getTargetConstant(0, dl, MVT::i32));
  }
  case ISD::STORE: {
    ptyType = false;
    ptyType = (N->getOperand(1).getValueType() == MVT::iPTR) ?
        true : false;
    break;
  }

}

  SDNode *ResNode = SelectCode(N);

  DEBUG(errs() << "=> ");
  if (ResNode == nullptr || ResNode == N)
    DEBUG(N->dump(CurDAG));
  else
    DEBUG(ResNode->dump(CurDAG));
  DEBUG(errs() << "\n");
  return ResNode;
}
/// createTriCoreISelDag - This pass converts a legalized DAG into a
/// TriCore-specific DAG, ready for instruction scheduling.
///
FunctionPass *llvm::createTriCoreISelDag(TriCoreTargetMachine &TM,
    CodeGenOpt::Level OptLevel) {
  return new TriCoreDAGToDAGISel(TM, OptLevel);
}
