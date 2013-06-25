
var __assert = require("assert")
var mp = require("msgpack")
var Duplex = require("stream").Duplex

var protocol = require("./protocol")
var RPC = protocol.RPC
var _RPC = protocol._RPC

var util = require("./util")

var dbg = 0

function Session(){
  this._id = null
  this.owner = null
  this._hdl = {}
  Duplex.call(this)
  util.bindHandlers(this.hdl,this._hdl,this)
}

Session.prototype = {
  __proto__:Duplex.prototype,
  
  // using .push() provided by Duplex
  
  choke:function(){
    __assert(!this.choked)
    this.choked = true
    this.push(null)
  },
  pushError:function(code,message){
    var e = new Error(message)
    e.code = code
    this.emit("error",e)
    this.close()
  },

  _read:function(n){
    // start reading. a no-op, in fact
  },

  _write:function(chunk,encoding,cb){
    if(Buffer.isBuffer(chunk)){
      var msg = mp.pack([_RPC.chunk,this._id,[chunk]])
    } else {
      __assert(typeof chunk === "string"
               && typeof encoding === "string")
      var msg = mp.pack([_RPC.chunk,this._id,[new Buffer(chunk,encoding)]])
    }
    this.owner._handle.send(msg)
  },

  end:function(){
    var r = Duplex.prototype.end.apply(this,arguments)
    this.owner._handle.send(mp.pack([_RPC.choke,this._id,[]]))
    return r
  },

  error:function(code,message){
    var hdl = this.owner._handle
    hdl.send(mp.pack([_RPC.error,this._id,[code,message]]))
    this.close()
  },

  close:function(){
    this._closed = true
  }

}

module.exports = {
  Session:Session
}

