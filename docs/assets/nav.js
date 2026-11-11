/* CornerstoneDesigner 用户手册 —— 侧栏导航注入 */
(function () {
  var SCRIPT = document.getElementById("nav-data");
  var CUR = (SCRIPT && SCRIPT.getAttribute("data-current")) || "";
  var NAV = [
    ["开始使用", [
      ["index.html", "1 手册首页"],
      ["01-overview.html", "2 设计器概览"],
      ["02-quick-start.html", "3 快速开始"],
      ["03-interface.html", "4 界面总览"],
      ["04-concepts.html", "5 核心概念"]
    ]],
    ["设计界面", [
      ["05-project-files.html", "6 工程与文件"],
      ["06-toolbox.html", "7 工具箱与放置"],
      ["07-canvas.html", "8 画布编辑"],
      ["08-tree.html", "9 控件树"],
      ["09-properties.html", "10 属性面板"],
      ["10-events.html", "11 事件面板"]
    ]],
    ["控件使用", [
      ["12-controls-general.html", "12 控件通用操作"],
      ["12-controls-basic.html", "13 基础与输入控件"],
      ["12-controls-container.html", "14 容器与布局控件"],
      ["12-controls-list.html", "15 列表、树与选择控件"],
      ["12-controls-menu.html", "16 菜单、状态栏与右键菜单"],
      ["12-controls-media.html", "17 图片、动画与绘制"]
    ]],
    ["预览与复用", [
      ["11-preview.html", "18 预览"],
      ["12-resources.html", "19 资源管理"],
      ["13-components.html", "20 组件系统"]
    ]],
    ["主题", [
      ["15-theme-system.html", "21 主题系统"],
      ["16-first-theme.html", "22 第一次使用主题"],
      ["17-theme-manager.html", "23 主题管理器"],
      ["18-theme-selectors.html", "24 选择器"],
      ["19-theme-declarations.html", "25 声明"],
      ["20-theme-variables.html", "26 变量"],
      ["21-theme-classes.html", "27 类"],
      ["22-theme-variants.html", "28 变体"],
      ["23-theme-media.html", "29 媒体条件"],
      ["24-theme-containers.html", "30 容器条件"],
      ["25-theme-example.html", "31 完整示例"],
      ["26-theme-notes.html", "32 其它说明"]
    ]],
    ["交付", [
      ["27-export.html", "33 导出与集成"],
      ["28-find-control.html", "34 在程序代码中找到控件"],
      ["29-read-write-controls.html", "35 读写控件数据"],
      ["30-json-actions.html", "36 注册 JSON 动作"],
      ["31-bind-events.html", "37 绑定控件事件"],
      ["32-event-differences.html", "38 JSON 动作与事件绑定的区别"],
      ["33-runtime-switch.html", "39 运行时切换事件逻辑"],
      ["34-access-in-callback.html", "40 回调中访问控件"],
      ["35-minimal-example.html", "41 最小完整示例"]
    ]],
    ["效率与支持", [
      ["36-shortcuts.html", "42 快捷键与效率"],
      ["37-troubleshooting.html", "43 故障排查与 FAQ"],
      ["38-tutorials.html", "44 实战教程"]
    ]],
    ["附录", [
      ["appendix-a-controls.html", "A 控件参考"],
      ["appendix-b-properties.html", "B 属性速查"],
      ["appendix-c-events.html", "C 事件速查"],
      ["appendix-d-theme.html", "D 主题语法参考"],
      ["appendix-e-files.html", "E 文件格式"],
      ["appendix-f-glossary.html", "F 术语表"],
      ["appendix-g-status.html", "G 当前限制与功能状态"]
    ]]
  ];
  function esc(s) { return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;"); }
  function renderNav() {
    var side = document.getElementById("sidebar");
    if (!side) return;
    var saved = {};
    try { saved = JSON.parse(localStorage.getItem("cdocsNavCollapsed") || "{}") || {}; } catch (e) {}
    var html = '<div class="brand"><a href="index.html"><img src="assets/logo.svg" alt="CornerstoneDesigner"><span>CornerstoneDesigner</span></a></div>';
    for (var i = 0; i < NAV.length; i++) {
      var sec = NAV[i];
      var collapsed = saved[i] === true ? " collapsed" : "";
      html += '<div class="sec' + collapsed + '" data-sec="' + i + '">' + esc(sec[0]) + '<span class="tw">▾</span></div>';
      html += '<div class="sec-items' + collapsed + '" data-sec="' + i + '">';
      for (var j = 0; j < sec[1].length; j++) {
        var item = sec[1][j];
        var cls = "item" + (item[0] === CUR ? " active" : "");
        html += '<a class="' + cls + '" href="' + item[0] + '">' + esc(item[1]) + '</a>';
      }
      html += '</div>';
    }
    html += '<div class="nav-foot">CornerstoneDesigner 用户手册<br><span>与 design/ 设计文档互补</span></div>';
    side.innerHTML = html;
    side.addEventListener("click", function (e) {
      var t = e.target;
      while (t && t !== side) {
        var cn = t.className;
        if (cn === "sec" || (typeof cn === "string" && cn.indexOf("sec ") === 0)) break;
        t = t.parentNode;
      }
      if (!t || t === side) return;
      var idx = t.getAttribute("data-sec");
      var collapsed = String(t.className).indexOf("collapsed") >= 0;
      for (var n = 0; n < side.children.length; n++) {
        var el = side.children[n];
        if (el.getAttribute && el.getAttribute("data-sec") === idx) {
          if (el.className === "sec" || String(el.className).indexOf("sec ") === 0)
            el.className = "sec" + (collapsed ? "" : " collapsed");
          else if (String(el.className).indexOf("sec-items") === 0)
            el.className = "sec-items" + (collapsed ? "" : " collapsed");
        }
      }
      saved[idx] = collapsed ? false : true;
      try { localStorage.setItem("cdocsNavCollapsed", JSON.stringify(saved)); } catch (err) {}
    });
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", renderNav);
  else renderNav();
})();
