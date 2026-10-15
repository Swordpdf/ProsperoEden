-- SPDX-License-Identifier: GPL-3.0-or-later
-- A neutral example using the same stable API as separately distributed launchers.
local games,index,time={},1,0
local message=""
local pending=false
local function action(name,args)
    local result=host.action(name,args or {})
    if result.ok==false then message=result.error or "Action failed" end
    return result
end
return {
    init=function() end,
    update=function(event)
        time=time+event.dt
        if time>.25 then time=0;games=host.query("library").games end
        if pending then
            local state=host.query("status").launch
            if state.stage=="ready" then action("launch.commit");pending=false
            elseif state.stage=="syncing" then message="Synchronizing saves. Circle cancels." end
        end
    end,
    input=function(event)
        if event.key=="options" then action("launcher.picker")
        elseif event.key=="circle" and pending then action("launch.cancel");pending=false
        elseif event.key=="left" then index=math.max(1,index-1)
        elseif event.key=="right" then index=math.min(#games,index+1)
        elseif event.key=="up" then index=math.max(1,index-4)
        elseif event.key=="down" then index=math.min(#games,index+4)
        elseif event.key=="cross" and games[index] then pending=action("launch.request",{game=games[index].handle}).ok end
    end,
    draw=function()
        host.draw("rect",{x=0,y=0,w=1920,h=1080,color=0x17212b})
        host.draw("text",{text="Grid Example",x=100,y=115,size=48,color=0xffffff})
        local top=math.max(0,math.floor((index-1)/4)-1)
        for i,g in ipairs(games) do
            local row=math.floor((i-1)/4)-top
            if row>=0 and row<2 then
                local x,y=100+((i-1)%4)*440,200+row*370
                host.draw("cover",{game=g.handle,x=x,y=y,w=300,h=300})
                if i==index then host.draw("border",{x=x-5,y=y-5,w=310,h=310,color=0,alpha=0,border=5,edge=0x6dd5ed}) end
                host.draw("text",{text=g.name,x=x,y=y+335,size=26,width=390,color=0xffffff})
            end
        end
        host.draw("text",{text=message,x=100,y=985,size=24,color=0xffffff})
        host.draw("text",{text="Directions: browse   Cross: start   Options: choose launcher",x=100,y=1040,size=24,color=0xb8c6d3})
    end
}
