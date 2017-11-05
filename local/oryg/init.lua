macAddr = "DE:AD:BE:EF:00:21"
hostname = "Test"

function globalErrorHandler(err)
    if log ~= nil then
        log(debug.traceback(err))
    else
        print(debug.traceback(err))
    end
end

wifiCallbacks = {}
safeMode = false

dofile("time.lc")
dofile("core.lc")

function core() 
    local _,reason = node.bootreason()
    if reason >= 1 and reason <=3 then 
        log("SAFE MODE")
        safeMode = true
    else
        xpcall(function() dofile("user.lua") end, globalErrorHandler)
    end
end

xpcall(core, globalErrorHandler)