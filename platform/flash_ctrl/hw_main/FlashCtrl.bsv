`include "ConnectalProjectConfig.bsv"

import FIFOF::*;
import FIFO::*;
import BRAMFIFO::*;
import BRAM::*;
import GetPut::*;
import ClientServer::*;
import Vector::*;
import Clocks::*;
import Leds::*;

import Pipe::*;

import AuroraGearbox::*;
import AuroraCommon::*;
import AuroraIntraFmc::*;
import ControllerTypes::*;
import StreamingSerDes::*;

// defined in ControllerTypes.bsv
/*
interface FlashCtrlUser;
	method Action sendCmd (FlashCmd cmd);
	method Action writeWord (Bit#(128) data, TagT tag);
	method ActionValue#(Tuple2#(Bit#(128), TagT)) readWord ();
	method ActionValue#(TagT) writeDataReq();
	method ActionValue#(Tuple2#(TagT, StatusT)) ackStatus ();
endinterface
*/
typedef TMul#(2, TAdd#(128, TLog#(NumTags))) SerInSz;

interface FCDebug;
	method Tuple2#(DataIfc, PacketType) debugRecPacket();
	method Tuple4#(Bit#(32), Bit#(32), Bit#(32), Bit#(32)) getDebugCnts;
`ifdef LED_AURORA
	method Bit#(LedsWidth) getAuroraStatus; // Chanwoo
`endif
endinterface

interface FlashCtrlIfc;
	interface FlashCtrlUser user;
	interface Aurora_Pins#(4) aurora;
	interface AuroraStatus#(4) auroraStatus;
	interface FCDebug debug;
endinterface

(* synthesize *)
module mkFlashCtrl0#(Clock gt_clk_p, Clock gt_clk_n, Clock clk110, Reset rst110)(FlashCtrlIfc);
	let _m <- mkFlashCtrl(True, gt_clk_p, gt_clk_n, clk110, rst110); 
	return _m;
endmodule

`ifdef TWO_FLASH_CARDS
(* synthesize *)
module mkFlashCtrl1#(Clock gt_clk_p, Clock gt_clk_n, Clock clk110, Reset rst110)(FlashCtrlIfc);
	let _m <- mkFlashCtrl(False, gt_clk_p, gt_clk_n, clk110, rst110); 
	return _m;
endmodule
`endif

module mkFlashCtrl#(Bool isFMC0,
	Clock gt_clk_p, Clock gt_clk_n, Clock clk110, Reset rst110) (FlashCtrlIfc);

	AuroraIfc auroraIntra;

`ifdef TWO_FLASH_CARDS
	if (isFMC0)
		auroraIntra <- mkAuroraIntra0(gt_clk_p, gt_clk_n, clk110, rst110);
	else
		auroraIntra <- mkAuroraIntra1(gt_clk_p, gt_clk_n, clk110, rst110);
`else
	if (!isFMC0) error("Turn on TWO_FLASH_CARDS to use the second slot");
	auroraIntra <- mkAuroraIntra0(gt_clk_p, gt_clk_n, clk110, rst110);
`endif

	FIFO#(FlashCmd) flashCmdQ <- mkSizedFIFO(16); //should not have back pressure
	FIFO#(Tuple2#(Bit#(128), TagT)) wrDataQ <- mkSizedFIFO(16); //TODO size?
	FIFO#(Tuple2#(Bit#(128), TagT)) rdDataQ <- mkSizedFIFO(16); //TODO size?
	FIFO#(TagT) wrReqQ <- mkSizedFIFO(valueOf(NumTags)); //TODO size?
	FIFO#(Tuple2#(TagT, StatusT)) ackQ <- mkSizedFIFO(valueOf(NumTags)); //TODO size?

	Reg#(Tuple2#(DataIfc, PacketType)) debugRecPacketV <- mkRegU();

	StreamingSerializerIfc#(Bit#(SerInSz), Bit#(TSub#(DataIfcSz,1))) ser <- mkStreamingSerializer();
	StreamingDeserializerIfc#(Bit#(TSub#(DataIfcSz,1)), Bit#(SerInSz)) des <- mkStreamingDeserializer();
	Reg#(Bit#(SerInSz)) serReg <- mkReg(0);
	Reg#(Bit#(4)) phase <- mkReg(0);

	//SEND V7 -> A7 rules: sendCmd and write data
	//Prioritize sendCmd over writeData
	(* descending_urgency = "forwardCmd, forwardWrData" *)
	rule forwardCmd;
		DataIfc data = zeroExtend(pack(flashCmdQ.first));
		PacketClass dataClass = F_CMD;
		PacketType dataType = zeroExtend(pack(dataClass));
		auroraIntra.send(data, dataType);
		flashCmdQ.deq();
	endrule

	rule packWrData;
		let wr = wrDataQ.first;
		wrDataQ.deq();
		Bit#(SerInSz) extWr = zeroExtend(pack(wr));
		if (phase==0) begin
			serReg <= extWr<<(valueOf(SerInSz)/2); 
			phase <= 1;
		end
		else begin
			Bit#(SerInSz) serData = serReg | extWr;
			$display("serializer enq: %x", serData);
			ser.enq(serData);
			phase <= 0;
		end
	endrule

	rule forwardWrData;
		let serData <- ser.deq;
		//DataIfc data = zeroExtend(pack(rd));
		PacketClass dataClass = F_DATA;
		PacketType dataType = zeroExtend(pack(dataClass));
		auroraIntra.send(pack(serData), dataType);
		//debugFwdCnt <= debugFwdCnt + 1;
	endrule

	//RECEIVE A7 -> V7 rules
	FIFO#(Tuple2#(DataIfc, PacketType)) receiveQ <- mkFIFO();
	rule receivePacketBuf;
		let typedData <- auroraIntra.receive();
		receiveQ.enq(typedData);
	endrule

	rule receivePacket;
		let typedData = receiveQ.first;
		receiveQ.deq;
		//let typedData <- auroraIntra.receive();
		debugRecPacketV <= typedData;
		DataIfc data = tpl_1(typedData);
		PacketType dataType = tpl_2(typedData);
		PacketClass dataClass = unpack(truncate(dataType)); 
		if (dataClass == F_DATA) begin
			Tuple2#(Bit#(TSub#(DataIfcSz,1)), Bool) desData = unpack(data);
			des.enq(tpl_1(desData), tpl_2(desData));
			$display("Received data burst");
			//rdDataQ.enq(unpack(truncate(data))); //TODO need to repack bursts
		end
		else if (dataClass == F_WR_REQ) begin
			TagT tag = unpack(truncate(data));
			wrReqQ.enq(tag); 
		end
		else if (dataClass == F_ACK) begin 
			Tuple2#(TagT, StatusT) ack = unpack(truncate(data));
			ackQ.enq(ack);
		end
		else begin
			$display("**ERROR: Unknown Packet Type"); //TODO go into err state
		end
		// Possible scenerio: pcie can't drain rdata fast enough? acks or wr_req 
		// can't be received either. but host always has space before issuing
		// rdata, so it might be ok? it drains eventually. 
	endrule
	
	Reg#(Bit#(4)) phaseRec <- mkReg(0);
	FIFO#(Bit#(SerInSz)) desPipe <- mkFIFO();
	rule desPipeline;
		let data <- des.deq;
		desPipe.enq(data);
	endrule

	rule fwdDataFromController; 
		Bit#(SerInSz) data = desPipe.first;
		if (phaseRec==0) begin
			Tuple2#(Bit#(128), TagT) fwdData = unpack( truncateLSB(data) );
			$display("tag = %x, data = %x", tpl_2(fwdData), tpl_1(fwdData));
			rdDataQ.enq(fwdData); 
			phaseRec <= 1;
		end
		else begin
			desPipe.deq;
			Tuple2#(Bit#(128), TagT) fwdData = unpack( truncate(data) );
			$display("tag = %x, data = %x", tpl_2(fwdData), tpl_1(fwdData));
			rdDataQ.enq(fwdData); 
			phaseRec <= 0;
		end
	endrule

	//bsim testing
	/*
	Reg#(Bit#(32)) dataReg <- mkReg(0);
	rule sendAurora;
		dataReg <= dataReg + 1;
		auroraIntra.send(zeroExtend(dataReg), 0);
		$display("aurora SEND data = %x", dataReg);
	endrule

	//receive slowly
	Integer recDelay = 1000;
	Reg#(Bit#(32)) delayReg <- mkReg(fromInteger(recDelay));
	rule receiveAurora;
		if (delayReg == 0) begin
			let typedData <- auroraIntra.receive();
			let data = tpl_1(typedData);
			$display("aurora RECEIVED data = %x", data);
			delayReg <= fromInteger(recDelay);
		end
		else begin
			delayReg <= delayReg - 1;
		end
	endrule
	*/	

	interface FlashCtrlUser user;
		method Action sendCmd (FlashCmd cmd); 
			flashCmdQ.enq(cmd);
		endmethod

		method Action writeWord (Tuple2#(Bit#(128), TagT) taggedData);
			wrDataQ.enq(taggedData);
		endmethod
			
		method ActionValue#(Tuple2#(Bit#(128), TagT)) readWord ();
			rdDataQ.deq();
			return rdDataQ.first();
		endmethod

		method ActionValue#(TagT) writeDataReq();
			wrReqQ.deq();
			return wrReqQ.first();
		endmethod

		method ActionValue#(Tuple2#(TagT, StatusT)) ackStatus ();
			ackQ.deq();
			return ackQ.first();
		endmethod
	endinterface

	interface FCDebug debug;
		method Tuple2#(DataIfc, PacketType) debugRecPacket();
			return debugRecPacketV;
		endmethod
		method Tuple4#(Bit#(32), Bit#(32), Bit#(32), Bit#(32)) getDebugCnts = auroraIntra.getDebugCnts;
`ifdef LED_AURORA
		method Bit#(LedsWidth) getAuroraStatus; //Chanwoo
			Bit#(LedsWidth) ret = 0;
			ret[0] = auroraIntra.status.channel_up;
			ret[4:1] = auroraIntra.status.lane_up;
			ret[5] = auroraIntra.status.hard_err;
			ret[6] = auroraIntra.status.soft_err;
			return ret;
		endmethod
`endif
	endinterface

	interface Aurora_Pins aurora = auroraIntra.aurora;
	interface AuroraStatus auroraStatus = auroraIntra.status;

endmodule
