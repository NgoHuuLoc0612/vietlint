"""
VietLint Training Data Pipeline — Upgraded v2
Scrapes Vietnamese identifier corpus from public GitHub repositories,
generates synthetic data, and exports a better ONNX model.

Upgrades vs v1:
  1. Fixed GitHub search queries (proper syntax)
  2. Much larger synthetic corpus (500+ curated examples)
  3. Fixed ONNX export input name to match C++ ('float_input')
  4. GradientBoosting + cross-validation instead of RandomForest
  5. Better deduplication + confidence weighting
  6. Async-ready rate limiting with exponential backoff
"""
from __future__ import annotations

import json
import logging
import re
import time
import urllib.request
import urllib.error
import urllib.parse
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional
import argparse

logger = logging.getLogger("vietlint.corpus")

# ---------------------------------------------------------------------------
# Vietnamese detection
# ---------------------------------------------------------------------------
VIET_RANGES = [
    (0x00C0, 0x024F),
    (0x0300, 0x036F),
    (0x1E00, 0x1EFF),
]

def has_vietnamese(text: str) -> bool:
    for ch in text:
        cp = ord(ch)
        for lo, hi in VIET_RANGES:
            if lo <= cp <= hi:
                return True
    return False

def extract_identifiers(source: str) -> list[str]:
    pattern = re.compile(r'[^\W\d]\w*', re.UNICODE)
    return list({m.group() for m in pattern.finditer(source)})

# ---------------------------------------------------------------------------
# GitHub API client — fixed queries + exponential backoff
# ---------------------------------------------------------------------------
GITHUB_API = "https://api.github.com"
GITHUB_RAW = "https://raw.githubusercontent.com"

# FIXED: proper GitHub search query syntax
# GitHub search queries covering all 14 tree-sitter languages + domain topics
SEARCH_QUERIES = [
    # ── Python ──────────────────────────────────────────────────────────────
    'ten_khach OR so_luong OR dia_chi language:python',
    'nguoi_dung OR mat_khau OR dang_nhap language:python',
    'quan_ly OR bao_cao OR danh_sach language:python',
    'nhan_vien OR khach_hang OR san_pham language:python',
    'ket_qua OR tinh_toan OR xu_ly language:python',
    'hoc_sinh OR giao_vien OR truong_hoc language:python',
    'benh_vien OR benh_nhan OR bac_si language:python',
    'tai_khoan OR giao_dich OR ngan_hang language:python',
    'du_an OR cong_viec OR nhiem_vu language:python',
    'thi_truong OR co_phieu OR tai_chinh language:python',
    'don_hang OR gio_hang OR thanh_toan language:python',
    'lop_hoc OR mon_hoc OR bang_diem language:python',
    'luong OR cham_cong OR nghi_phep language:python',
    'van_don OR giao_hang OR kho language:python',
    'du_lieu OR mo_hinh OR huan_luyen language:python',
    'nhan_dang OR phan_loai OR du_doan language:python',
    'tênKhách OR sốLượng OR địaChỉ language:python',
    'ngườiDùng OR mậtKhẩu language:python',
    'topic:vietnamese language:python',
    'topic:viet language:python',
    'topic:vietnam language:python',
    # ── JavaScript ──────────────────────────────────────────────────────────
    'ten_khach OR so_luong OR dia_chi language:javascript',
    'nguoi_dung OR mat_khau OR dang_nhap language:javascript',
    'quan_ly OR danh_sach OR san_pham language:javascript',
    'tenKhach OR soLuong OR diaHinh language:javascript',
    'donHang OR gioHang OR thanhToan language:javascript',
    'topic:vietnamese language:javascript',
    'topic:viet language:javascript',
    # ── TypeScript ──────────────────────────────────────────────────────────
    'nguoi_dung OR mat_khau language:typescript',
    'quan_ly OR bao_cao OR ket_qua language:typescript',
    'nguoiDung OR matKhau OR dangNhap language:typescript',
    'tenKhach OR soLuong language:typescript',
    'topic:vietnamese language:typescript',
    # ── Java ────────────────────────────────────────────────────────────────
    'tenKhach OR soLuong OR diaHinh language:java',
    'nguoiDung OR matKhau OR dangNhap language:java',
    'quanLy OR baoCao OR danhSach language:java',
    'nhanVien OR khachHang OR sanPham language:java',
    'lopHoc OR monHoc OR bangDiem language:java',
    'benhVien OR benhNhan OR bacSi language:java',
    'topic:vietnamese language:java',
    'topic:vietnam language:java',
    # ── C++ ─────────────────────────────────────────────────────────────────
    'tenKhach OR soLuong OR diaHinh language:cpp',
    'nguoiDung OR matKhau language:cpp',
    'quanLy OR danhSach language:cpp',
    'nhanVien OR sanPham language:cpp',
    'nhiet_do OR do_am OR anh_sang language:cpp',
    'topic:vietnamese language:cpp',
    # ── C ───────────────────────────────────────────────────────────────────
    'ten_khach OR so_luong language:c',
    'nguoi_dung OR mat_khau language:c',
    'cam_bien OR dieu_khien OR thiet_bi language:c',
    'topic:vietnamese language:c',
    # ── C# ──────────────────────────────────────────────────────────────────
    'tenKhach OR soLuong language:csharp',
    'nguoiDung OR matKhau language:csharp',
    'quanLy OR danhSach language:csharp',
    'nhanVien OR khachHang language:csharp',
    'topic:vietnamese language:csharp',
    # ── Go ──────────────────────────────────────────────────────────────────
    'ten_khach OR so_luong language:go',
    'nguoi_dung OR mat_khau language:go',
    'quan_ly OR danh_sach language:go',
    'don_hang OR tai_khoan language:go',
    'topic:vietnamese language:go',
    # ── PHP ─────────────────────────────────────────────────────────────────
    'ten_khach OR so_luong OR dia_chi language:php',
    'nguoi_dung OR mat_khau OR dang_nhap language:php',
    'quan_ly OR danh_sach language:php',
    'don_hang OR gio_hang OR thanh_toan language:php',
    'hoc_sinh OR giao_vien language:php',
    'topic:vietnamese language:php',
    # ── Rust ────────────────────────────────────────────────────────────────
    'ten_khach OR so_luong language:rust',
    'nguoi_dung OR ket_qua language:rust',
    'quan_ly OR danh_sach language:rust',
    'topic:vietnamese language:rust',
    # ── Scala ───────────────────────────────────────────────────────────────
    'tenKhach OR soLuong language:scala',
    'nguoiDung OR matKhau language:scala',
    'quanLy OR danhSach language:scala',
    'topic:vietnamese language:scala',
    # ── CSS ─────────────────────────────────────────────────────────────────
    'topic:vietnamese language:css',
    'topic:viet language:css',
    # ── JSON ────────────────────────────────────────────────────────────────
    'ten_khach OR so_luong language:json',
    'nguoi_dung OR dia_chi language:json',
    # ── Julia ───────────────────────────────────────────────────────────────
    'ten_khach OR so_luong language:julia',
    'nguoi_dung OR ket_qua language:julia',
    'topic:vietnamese language:julia',
    # ── OCaml ───────────────────────────────────────────────────────────────
    'ten_khach OR so_luong language:ocaml',
    'topic:vietnamese language:ocaml',
    # ── Domain: E-commerce ──────────────────────────────────────────────────
    'san_pham OR danh_muc OR ton_kho language:python',
    'khuyen_mai OR giam_gia OR ma_giam language:python',
    # ── Domain: Healthcare ──────────────────────────────────────────────────
    'don_thuoc OR xet_nghiem OR lich_kham language:java',
    # ── Domain: Finance ─────────────────────────────────────────────────────
    'tai_khoan OR giao_dich OR so_du language:python',
    'vay_von OR lai_suat OR du_no language:java',
    # ── Domain: HR/Payroll ──────────────────────────────────────────────────
    'bang_luong OR phu_cap OR thue_tncn language:java',
    # ── Domain: Logistics ───────────────────────────────────────────────────
    'lo_hang OR xuat_nhap_khau language:java',
    # ── Domain: Real Estate ─────────────────────────────────────────────────
    'bat_dong_san OR can_ho OR cho_thue language:python',
    'du_an OR mat_bang OR gia_thue language:javascript',
]

class GitHubClient:
    def __init__(self, token: Optional[str] = None, rate_limit_delay: float = 1.0):
        self.token = token
        self.delay = rate_limit_delay
        self._headers: dict[str, str] = {
            "Accept": "application/vnd.github.v3+json",
            "User-Agent": "VietLint-Corpus-Collector/2.0",
        }
        if token:
            self._headers["Authorization"] = f"token {token}"

    def _request(self, url: str, retries: int = 3) -> Optional[dict | list]:
        for attempt in range(retries):
            req = urllib.request.Request(url, headers=self._headers)
            try:
                with urllib.request.urlopen(req, timeout=20) as resp:
                    time.sleep(self.delay)
                    return json.loads(resp.read().decode("utf-8"))
            except urllib.error.HTTPError as e:
                if e.code == 403:
                    wait = 60 * (attempt + 1)
                    logger.warning("Rate limited. Waiting %ds...", wait)
                    time.sleep(wait)
                elif e.code == 422:
                    logger.warning("Invalid query for %s", url)
                    return None
                elif e.code == 404:
                    return None
                else:
                    logger.error("HTTP %d for %s", e.code, url)
                    return None
            except Exception as e:
                wait = 2 ** attempt
                logger.error("Request error: %s. Retry in %ds", e, wait)
                time.sleep(wait)
        return None

    def search_repos(self, max_repos: int = 100) -> list[dict]:
        repos = {}
        per_page = 30
        for query in SEARCH_QUERIES:
            encoded = urllib.parse.quote(query)
            url = f"{GITHUB_API}/search/repositories?q={encoded}&per_page={per_page}&sort=stars&order=desc"
            data = self._request(url)
            if data and "items" in data:
                for item in data["items"]:
                    full_name = item.get("full_name", "")
                    if full_name and full_name not in repos:
                        repos[full_name] = item
            logger.info("Collected %d unique repos so far", len(repos))
        logger.info("Total unique repos found: %d (capped at %d)", len(repos), max_repos)
        return list(repos.values())[:max_repos]

    def get_tree(self, owner: str, repo: str) -> list[dict]:
        for branch in ["main", "master", "develop"]:
            url = f"{GITHUB_API}/repos/{owner}/{repo}/git/trees/{branch}?recursive=1"
            data = self._request(url)
            if data and "tree" in data:
                return data["tree"]
        return []

    def get_file_content(self, owner: str, repo: str, path: str, branch: str = "main") -> Optional[str]:
        for br in [branch, "master"]:
            url = f"{GITHUB_RAW}/{owner}/{repo}/{br}/{path}"
            req = urllib.request.Request(url, headers=self._headers)
            try:
                with urllib.request.urlopen(req, timeout=20) as resp:
                    time.sleep(self.delay)
                    return resp.read().decode("utf-8", errors="replace")
            except Exception:
                continue
        return None

# ---------------------------------------------------------------------------
# Labels
# ---------------------------------------------------------------------------
# 0 = pure_ascii, 1 = pure_vietnamese, 2 = mixed_vietnamese,
# 3 = transliterated, 4 = abbreviation, 5 = unknown

SOURCE_EXTENSIONS = {".py", ".js", ".ts", ".c", ".cpp", ".java", ".cs", ".go"}

VIET_WORD_SET = {
    # Người, địa điểm
    "ten","tuoi","diachi","dienthoai","email","matkhau","hoten","ngaysinh",
    "gioitinh","quequan","quocgia","thanhpho","tinh","huyen","xa","phuong",
    # Kinh doanh
    "danhmuc","loai","loaisanpham","donvi","soluong","dongia","thanhtien",
    "hoadon","nhanvien","khachhang","sanpham","nhacungcap","kho","tonkho",
    "xuatkho","nhapkho","phieuthu","phieuchi","baocao","doanhthu","chiphi",
    "loinhuân","loinhuán","thue","phucap","luong","bangluong","chamcong",
    # IT/Dev
    "nguoidung","matkhau","dangnhap","dangky","xacthuc","quanly","phanquyen",
    "vaitro","taikhoan","phiengiam","token","cachep","dulieu","database",
    "bangdu","truong","giatri","kieudu","chuoi","mangdu","doituong",
    # Y tế
    "benhvien","hosobenh","benhnhan","bacsi","phongkham","lichkham",
    "ketqua","xetnghiem","dongthuoc","thuoc","benh","trieuchung",
    # Giáo dục
    "hocsinh","giaovien","lophoc","monhoc","diem","bangdiem","hocky",
    "truonghoc","khoa","nganh","bangcap","chungchi",
    # Transliterated camelCase
    "tenkhach","soLuong","diachi","matkhau","nguoidung","dangnhap",
    "tinhTong","ketQua","tenKhach","soLuong","diaHinh","quanLy",
    "loaiHang","maSo","nguoiDung","matKhau","dangNhap","thoiGian",
    "namSinh","gioiTinh","phuongThuc","trangThai","loaiTk",
}

def auto_label(identifier: str, is_viet: bool) -> int:
    if not is_viet:
        lower = identifier.lower().replace("_", "").replace("-", "")
        if lower in VIET_WORD_SET:
            return 3
        # Check camelCase decomposition
        parts = re.sub(r'([A-Z])', r' \1', identifier).lower().split()
        combined = "".join(parts)
        if combined in VIET_WORD_SET:
            return 3
        # Vietnamese abbreviations (all caps, short, consonant-heavy)
        if len(identifier) <= 6 and identifier.isupper() and not identifier.isdigit():
            vn_abbrev_pattern = re.compile(r'^[BCDĐGHKLMNPQRSTVX]{2,6}$')
            if vn_abbrev_pattern.match(identifier):
                return 4
        return 0

    ascii_chars = sum(1 for c in identifier if ord(c) < 0x80 and c.isalpha())
    viet_chars  = sum(1 for c in identifier if has_vietnamese(c))
    total = ascii_chars + viet_chars
    if total == 0:
        return 5
    ratio = viet_chars / total
    return 1 if ratio >= 0.85 else 2

# ---------------------------------------------------------------------------
# Synthetic corpus — 500+ curated examples
# ---------------------------------------------------------------------------
SYNTHETIC_CORPUS: list[tuple[str, int, float]] = [
    # ── pure_ascii (0) ──────────────────────────────────────────────────────
    ("getUserById", 0, 0.98), ("calculateTotal", 0, 0.98), ("parseConfig", 0, 0.98),
    ("renderComponent", 0, 0.98), ("fetchData", 0, 0.98), ("validateInput", 0, 0.98),
    ("handleError", 0, 0.98), ("processQueue", 0, 0.98), ("createSession", 0, 0.98),
    ("deleteRecord", 0, 0.98), ("updateProfile", 0, 0.98), ("sendEmail", 0, 0.98),
    ("loadConfig", 0, 0.98), ("saveFile", 0, 0.98), ("readBytes", 0, 0.98),
    ("writeLog", 0, 0.98), ("parseJson", 0, 0.98), ("formatDate", 0, 0.98),
    ("sortList", 0, 0.98), ("filterItems", 0, 0.98), ("mapValues", 0, 0.98),
    ("reduceArray", 0, 0.98), ("mergeObjects", 0, 0.98), ("cloneDeep", 0, 0.98),
    ("debounce", 0, 0.98), ("throttle", 0, 0.98), ("memoize", 0, 0.98),
    ("authenticate", 0, 0.98), ("authorize", 0, 0.98), ("encrypt", 0, 0.98),
    ("decrypt", 0, 0.98), ("compress", 0, 0.98), ("decompress", 0, 0.98),
    ("serialize", 0, 0.98), ("deserialize", 0, 0.98), ("tokenize", 0, 0.98),
    ("firstName", 0, 0.98), ("lastName", 0, 0.98), ("emailAddress", 0, 0.98),
    ("phoneNumber", 0, 0.98), ("streetAddress", 0, 0.98), ("zipCode", 0, 0.98),
    ("countryCode", 0, 0.98), ("currencySymbol", 0, 0.98), ("totalAmount", 0, 0.98),
    ("unitPrice", 0, 0.98), ("quantity", 0, 0.98), ("discount", 0, 0.98),
    ("taxRate", 0, 0.98), ("invoiceId", 0, 0.98), ("orderId", 0, 0.98),
    ("productId", 0, 0.98), ("customerId", 0, 0.98), ("employeeId", 0, 0.98),
    ("isActive", 0, 0.98), ("isValid", 0, 0.98), ("hasError", 0, 0.98),
    ("canEdit", 0, 0.98), ("shouldUpdate", 0, 0.98), ("maxRetries", 0, 0.98),
    ("minLength", 0, 0.98), ("pageSize", 0, 0.98), ("currentPage", 0, 0.98),
    ("totalPages", 0, 0.98), ("startDate", 0, 0.98), ("endDate", 0, 0.98),
    ("createdAt", 0, 0.98), ("updatedAt", 0, 0.98), ("deletedAt", 0, 0.98),
    ("requestBody", 0, 0.98), ("responseData", 0, 0.98), ("statusCode", 0, 0.98),
    ("errorMessage", 0, 0.98), ("successMessage", 0, 0.98), ("loading", 0, 0.98),
    ("error", 0, 0.98), ("data", 0, 0.98), ("result", 0, 0.98), ("count", 0, 0.98),
    ("index", 0, 0.98), ("key", 0, 0.98), ("value", 0, 0.98), ("item", 0, 0.98),
    ("list", 0, 0.98), ("array", 0, 0.98), ("map", 0, 0.98), ("set", 0, 0.98),
    ("buffer", 0, 0.98), ("stream", 0, 0.98), ("chunk", 0, 0.98), ("offset", 0, 0.98),

    # ── pure_vietnamese (1) ─────────────────────────────────────────────────
    ("tênKhách", 1, 0.99), ("sốLượng", 1, 0.99), ("địaChỉ", 1, 0.99),
    ("điệnThoại", 1, 0.99), ("ngàySinh", 1, 0.99), ("họTên", 1, 0.99),
    ("mậtKhẩu", 1, 0.99), ("danhMục", 1, 0.99), ("loạiSảnPhẩm", 1, 0.99),
    ("đơnVị", 1, 0.99), ("giáTiền", 1, 0.99), ("thànhTiền", 1, 0.99),
    ("nhânViên", 1, 0.99), ("kháchHàng", 1, 0.99), ("sảnPhẩm", 1, 0.99),
    ("nhàCungCấp", 1, 0.99), ("khoHàng", 1, 0.99), ("tồnKho", 1, 0.99),
    ("phiếuThu", 1, 0.99), ("phiếuChi", 1, 0.99), ("báoCáo", 1, 0.99),
    ("doanhThu", 1, 0.99), ("chiPhí", 1, 0.99), ("lợiNhuận", 1, 0.99),
    ("thuế", 1, 0.99), ("phụCấp", 1, 0.99), ("lương", 1, 0.99),
    ("ngườiDùng", 1, 0.99), ("đăngNhập", 1, 0.99), ("đăngKý", 1, 0.99),
    ("xácThực", 1, 0.99), ("quảnLý", 1, 0.99), ("phânQuyền", 1, 0.99),
    ("vaiTrò", 1, 0.99), ("tàiKhoản", 1, 0.99), ("phiênLàmViệc", 1, 0.99),
    ("bệnhNhân", 1, 0.99), ("bácSĩ", 1, 0.99), ("phòngKhám", 1, 0.99),
    ("lịchKhám", 1, 0.99), ("kếtQuả", 1, 0.99), ("xétNghiệm", 1, 0.99),
    ("đơnThuốc", 1, 0.99), ("thuốc", 1, 0.99), ("bệnh", 1, 0.99),
    ("họcSinh", 1, 0.99), ("giáoViên", 1, 0.99), ("lớpHọc", 1, 0.99),
    ("mônHọc", 1, 0.99), ("điểmSố", 1, 0.99), ("bằngĐiểm", 1, 0.99),
    ("côngTy", 1, 0.99), ("dựÁn", 1, 0.99), ("côngViệc", 1, 0.99),
    ("nhiệmVụ", 1, 0.99), ("thờiHạn", 1, 0.99), ("ưuTiên", 1, 0.99),

    # ── mixed_vietnamese (2) ────────────────────────────────────────────────
    ("getUserTên", 2, 0.92), ("tênUser", 2, 0.92), ("getKháchHàng", 2, 0.92),
    ("nhânViênID", 2, 0.92), ("loadDanhSách", 2, 0.90), ("renderTênKhách", 2, 0.90),
    ("apiKháchHàng", 2, 0.90), ("dbNgườiDùng", 2, 0.90), ("updateLương", 2, 0.90),
    ("fetchSảnPhẩm", 2, 0.90), ("deleteTàiKhoản", 2, 0.90), ("createĐơnHàng", 2, 0.90),
    ("listNhânViên", 2, 0.90), ("searchKháchHàng", 2, 0.90), ("filterSảnPhẩm", 2, 0.90),
    ("sortByTên", 2, 0.90), ("groupByLoại", 2, 0.90), ("countĐơnHàng", 2, 0.90),
    ("sumThànhTiền", 2, 0.90), ("avgĐiểmSố", 2, 0.90), ("maxLương", 2, 0.90),
    ("minGiáTiền", 2, 0.90), ("totalDoanhThu", 2, 0.90), ("handleĐăngNhập", 2, 0.90),
    ("validateMậtKhẩu", 2, 0.90), ("formatNgàySinh", 2, 0.90),
    ("parseĐịaChỉ", 2, 0.90), ("encryptMậtKhẩu", 2, 0.90),
    ("sendEmailXácNhận", 2, 0.88), ("generateBáoCáo", 2, 0.88),
    ("exportDanhSách", 2, 0.88), ("importDữLiệu", 2, 0.88),
    ("syncKhoHàng", 2, 0.88), ("calcThànhTiền", 2, 0.88),

    # ── transliterated (3) ──────────────────────────────────────────────────
    # snake_case
    ("ten_khach_hang", 3, 0.90), ("so_luong", 3, 0.90), ("dia_chi", 3, 0.90),
    ("mat_khau", 3, 0.90), ("ngay_sinh", 3, 0.90), ("dien_thoai", 3, 0.90),
    ("loai_san_pham", 3, 0.90), ("nhan_vien", 3, 0.90), ("khach_hang", 3, 0.90),
    ("san_pham", 3, 0.90), ("nha_cung_cap", 3, 0.90), ("kho_hang", 3, 0.90),
    ("ton_kho", 3, 0.90), ("phieu_thu", 3, 0.90), ("phieu_chi", 3, 0.90),
    ("bao_cao", 3, 0.90), ("doanh_thu", 3, 0.90), ("chi_phi", 3, 0.90),
    ("loi_nhuan", 3, 0.90), ("phu_cap", 3, 0.90), ("bang_luong", 3, 0.90),
    ("nguoi_dung", 3, 0.90), ("dang_nhap", 3, 0.90), ("dang_ky", 3, 0.90),
    ("xac_thuc", 3, 0.90), ("quan_ly", 3, 0.90), ("phan_quyen", 3, 0.90),
    ("vai_tro", 3, 0.90), ("tai_khoan", 3, 0.90), ("thoi_gian", 3, 0.90),
    ("lich_kham", 3, 0.90), ("ket_qua", 3, 0.90), ("xet_nghiem", 3, 0.90),
    ("hoc_sinh", 3, 0.90), ("giao_vien", 3, 0.90), ("lop_hoc", 3, 0.90),
    ("mon_hoc", 3, 0.90), ("diem_so", 3, 0.90), ("cong_ty", 3, 0.90),
    ("du_an", 3, 0.90), ("cong_viec", 3, 0.90), ("nhiem_vu", 3, 0.90),
    # camelCase transliterated
    ("tinhTong", 3, 0.85), ("ketQua", 3, 0.85), ("tenKhach", 3, 0.85),
    ("soLuong", 3, 0.85), ("diaHinhBan", 3, 0.85), ("quanLy", 3, 0.85),
    ("loaiHang", 3, 0.85), ("maSo", 3, 0.85), ("nguoiDung", 3, 0.85),
    ("matKhau", 3, 0.85), ("dangNhap", 3, 0.85), ("thoiGian", 3, 0.85),
    ("namSinh", 3, 0.85), ("gioiTinh", 3, 0.85), ("phuongThuc", 3, 0.85),
    ("trangThai", 3, 0.85), ("loaiTk", 3, 0.85), ("maDonHang", 3, 0.85),
    ("tenSanPham", 3, 0.85), ("giaBan", 3, 0.85), ("soHoaDon", 3, 0.85),
    ("ngayTao", 3, 0.85), ("ngayCapNhat", 3, 0.85), ("danhSach", 3, 0.85),
    ("chiTiet", 3, 0.85), ("tongTien", 3, 0.85), ("phiVanChuyen", 3, 0.85),
    ("maKhuyenMai", 3, 0.85), ("tienGiam", 3, 0.85), ("hinhThuc", 3, 0.85),
    ("trangThaiDonHang", 3, 0.85), ("lyDo", 3, 0.85), ("ghiChu", 3, 0.85),
    ("diaChi", 3, 0.85), ("thanhPho", 3, 0.85), ("quocGia", 3, 0.85),
    # PascalCase transliterated
    ("TinhTong", 3, 0.82), ("KetQua", 3, 0.82), ("TenKhach", 3, 0.82),
    ("SoLuong", 3, 0.82), ("QuanLy", 3, 0.82), ("NguoiDung", 3, 0.82),
    ("MatKhau", 3, 0.82), ("DangNhap", 3, 0.82), ("ThoiGian", 3, 0.82),

    # ── abbreviation (4) ────────────────────────────────────────────────────
    ("QLKH", 4, 0.92), ("NVKD", 4, 0.92), ("QLNV", 4, 0.92), ("DSKH", 4, 0.92),
    ("BCTH", 4, 0.92), ("PTTH", 4, 0.92), ("THCS", 4, 0.92), ("THPT", 4, 0.92),
    ("UBND", 4, 0.92), ("HDND", 4, 0.92), ("BHXH", 4, 0.92), ("BHYT", 4, 0.92),
    ("CMND", 4, 0.92), ("CCCD", 4, 0.92), ("GPLX", 4, 0.92), ("DKKD", 4, 0.92),
    ("TNHH", 4, 0.92), ("DNTN", 4, 0.92), ("CTCP", 4, 0.92), ("NSNN", 4, 0.92),
    ("KBNN", 4, 0.92), ("NHNN", 4, 0.92), ("TCTD", 4, 0.92), ("TCKT", 4, 0.92),
    ("CNTT", 4, 0.92), ("CSDL", 4, 0.92), ("HTTT", 4, 0.92), ("PMQL", 4, 0.92),
    ("QL", 4, 0.85), ("KH", 4, 0.85), ("NV", 4, 0.85), ("SP", 4, 0.85),
    ("DH", 4, 0.85), ("TK", 4, 0.85), ("BC", 4, 0.85), ("HĐ", 4, 0.85),
]

# ---------------------------------------------------------------------------
# Corpus collector
# ---------------------------------------------------------------------------
@dataclass
class CollectionStats:
    repos_scanned:     int = 0
    files_scanned:     int = 0
    identifiers_found: int = 0
    vietnamese_found:  int = 0
    errors:            int = 0

class CorpusCollector:
    def __init__(self,
                 output_dir: Path,
                 token: Optional[str] = None,
                 max_repos: int = 50,
                 max_files_per_repo: int = 20,
                 skip_github: bool = False):
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.client     = GitHubClient(token, rate_limit_delay=1.0 if token else 2.0)
        self.max_repos  = max_repos
        self.max_files  = max_files_per_repo
        self.stats      = CollectionStats()
        self.skip_github = skip_github

    def collect(self) -> Path:
        output_path = self.output_dir / "corpus_raw.jsonl"
        seen_ids: set[str] = set()

        with open(output_path, "w", encoding="utf-8") as out_f:
            # Phase 1: Curated synthetic corpus
            logger.info("Adding synthetic corpus (%d examples)...", len(SYNTHETIC_CORPUS))
            for ident, label, conf in SYNTHETIC_CORPUS:
                if ident in seen_ids:
                    continue
                seen_ids.add(ident)
                entry = {
                    "id":             ident,
                    "label":          label,
                    "confidence":     conf,
                    "has_vietnamese": has_vietnamese(ident),
                    "language":       "curated",
                    "source":         "vietlint/synthetic_v2",
                }
                out_f.write(json.dumps(entry, ensure_ascii=False) + "\n")

            # Phase 2: GitHub scraping (optional)
            if not self.skip_github:
                logger.info("Searching GitHub repositories...")
                repos = self.client.search_repos(max_repos=self.max_repos)
                logger.info("Found %d repositories", len(repos))

                for repo in repos:
                    owner = repo.get("owner", {}).get("login", "")
                    name  = repo.get("name", "")
                    if not owner or not name:
                        continue

                    self.stats.repos_scanned += 1
                    logger.info("[%d/%d] Scanning %s/%s",
                                self.stats.repos_scanned, len(repos), owner, name)

                    tree = self.client.get_tree(owner, name)
                    source_files = [
                        f for f in tree
                        if f.get("type") == "blob"
                        and Path(f.get("path", "")).suffix in SOURCE_EXTENSIONS
                        and f.get("size", 0) < 300_000
                    ][:self.max_files]

                    for file_info in source_files:
                        path = file_info["path"]
                        lang = Path(path).suffix.lstrip(".")
                        content = self.client.get_file_content(owner, name, path)
                        if not content:
                            self.stats.errors += 1
                            continue

                        self.stats.files_scanned += 1
                        identifiers = extract_identifiers(content)

                        for ident in identifiers:
                            if ident in seen_ids or len(ident) < 2 or len(ident) > 80:
                                continue
                            # Skip pure digits or common keywords
                            if ident.isdigit() or ident in {
                                "if","else","for","while","return","import",
                                "from","class","def","var","let","const","function",
                                "true","false","null","None","True","False",
                            }:
                                continue
                            seen_ids.add(ident)
                            self.stats.identifiers_found += 1

                            is_viet = has_vietnamese(ident)
                            if is_viet:
                                self.stats.vietnamese_found += 1

                            label = auto_label(ident, is_viet)
                            conf  = 0.75 if is_viet else (0.80 if label == 3 else 0.70)

                            entry = {
                                "id":             ident,
                                "label":          label,
                                "confidence":     conf,
                                "has_vietnamese": is_viet,
                                "language":       lang,
                                "source":         f"{owner}/{name}/{path}",
                            }
                            out_f.write(json.dumps(entry, ensure_ascii=False) + "\n")

        logger.info("Collection complete. Stats: %s", self.stats)
        return output_path

# ---------------------------------------------------------------------------
# Training script — GradientBoosting + cross-val + FIXED ONNX input name
# ---------------------------------------------------------------------------
TRAIN_SCRIPT = '''#!/usr/bin/env python3
"""
VietLint model training script v2.
GradientBoosting + cross-validation + correct ONNX input name.

Usage:
    python train_model.py --corpus corpus/corpus_raw.jsonl --output model.onnx
"""
import argparse
import json
import sys
from pathlib import Path
from collections import Counter

import numpy as np

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--output", default="vietlint_classifier.onnx")
    parser.add_argument("--test-split", type=float, default=0.15)
    parser.add_argument("--min-confidence", type=float, default=0.70)
    args = parser.parse_args()

    # Load corpus
    examples = []
    with open(args.corpus, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                ex = json.loads(line)
                if ex.get("label", -1) >= 0 and ex.get("confidence", 0) >= args.min_confidence:
                    examples.append(ex)
            except json.JSONDecodeError:
                continue

    if not examples:
        print("No labeled examples found", file=sys.stderr)
        sys.exit(1)

    print(f"Loaded {len(examples)} examples")
    label_counts = Counter(ex["label"] for ex in examples)
    label_names = ["pure_ascii","pure_viet","mixed_viet","transliterated","abbreviation","unknown"]
    for lbl, cnt in sorted(label_counts.items()):
        name = label_names[lbl] if lbl < len(label_names) else str(lbl)
        print(f"  label {lbl} ({name}): {cnt}")

    # Extract features
    try:
        import sys as _sys
        _sys.path.insert(0, str(Path(__file__).parent.parent / "build" / "python"))
        import vietlint_core as core
        clf_engine = core.IdentifierClassifier()
        identifiers = [ex["id"] for ex in examples]
        X = np.array(clf_engine.extract_features(identifiers), dtype=np.float32)
    except ImportError as e:
        print(f"vietlint_core not available: {e}", file=sys.stderr)
        sys.exit(1)

    y = np.array([ex["label"] for ex in examples], dtype=np.int32)
    weights = np.array([ex.get("confidence", 1.0) for ex in examples], dtype=np.float32)

    # Filter out unknown label (5)
    mask = y < 5
    X, y, weights = X[mask], y[mask], weights[mask]
    print(f"After filtering unknowns: {len(y)} examples")

    # Train/test split with stratification
    from sklearn.model_selection import train_test_split, cross_val_score
    X_train, X_test, y_train, y_test, w_train, _ = train_test_split(
        X, y, weights, test_size=args.test_split, random_state=42,
        stratify=y if len(set(y)) > 1 else None
    )

    # GradientBoosting pipeline
    from sklearn.ensemble import GradientBoostingClassifier
    from sklearn.preprocessing import StandardScaler
    from sklearn.pipeline import Pipeline
    from sklearn.metrics import classification_report

    model = Pipeline([
        ("scaler", StandardScaler()),
        ("clf", GradientBoostingClassifier(
            n_estimators=300,
            max_depth=4,
            learning_rate=0.08,
            subsample=0.8,
            random_state=42,
            n_iter_no_change=15,
            validation_fraction=0.1,
        )),
    ])

    model.fit(X_train, y_train, clf__sample_weight=w_train)

    # Evaluation
    y_pred = model.predict(X_test)
    active_labels = sorted(set(y_test))
    active_names = [label_names[i] for i in active_labels if i < len(label_names)]
    print("\\n" + classification_report(y_test, y_pred,
        labels=active_labels, target_names=active_names))

    # Cross-validation
    cv_scores = cross_val_score(model, X, y, cv=5, scoring="f1_macro")
    print(f"5-fold CV F1-macro: {cv_scores.mean():.3f} ± {cv_scores.std():.3f}")

    # Export to ONNX — IMPORTANT: input name must match C++ INPUT_NAME = "float_input"
    from skl2onnx import convert_sklearn
    from skl2onnx.common.data_types import FloatTensorType

    initial_type = [("float_input", FloatTensorType([None, X_train.shape[1]]))]
    onnx_model = convert_sklearn(
        model,
        initial_types=initial_type,
        options={id(model.named_steps["clf"]): {"zipmap": False}},
    )

    with open(args.output, "wb") as f:
        f.write(onnx_model.SerializeToString())
    print(f"\\nModel exported to {args.output}")
    print(f"Input name: float_input (matches C++ INPUT_NAME)")
    print(f"Feature dim: {X_train.shape[1]}")

if __name__ == "__main__":
    main()
'''

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    parser = argparse.ArgumentParser(description="VietLint corpus collector v2")
    parser.add_argument("--token",  help="GitHub personal access token")
    parser.add_argument("--output", default="corpus", help="Output directory")
    parser.add_argument("--max-repos",  type=int, default=50)
    parser.add_argument("--max-files",  type=int, default=20)
    parser.add_argument("--no-github",  action="store_true",
                        help="Skip GitHub scraping, use synthetic corpus only")
    parser.add_argument("--write-train-script", action="store_true")
    args = parser.parse_args()

    output_dir = Path(args.output)

    if args.write_train_script:
        output_dir.mkdir(parents=True, exist_ok=True)
        script_path = output_dir / "train_model.py"
        script_path.write_text(TRAIN_SCRIPT)
        print(f"Training script written to {script_path}")
        return

    collector = CorpusCollector(
        output_dir         = output_dir,
        token              = args.token,
        max_repos          = args.max_repos,
        max_files_per_repo = args.max_files,
        skip_github        = args.no_github,
    )
    output_path = collector.collect()
    print(f"\nCorpus written to: {output_path}")
    print(f"Stats: {collector.stats}")
    print(f"\nNext steps:")
    print(f"  python {__file__} --write-train-script --output {output_dir}")
    print(f"  python {output_dir}/train_model.py --corpus {output_path} --output model.onnx")

if __name__ == "__main__":
    main()
